/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "dnnl.h"

#include "src/common/dnnl_thread.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "softmax/softmax.hpp"

namespace softmax {

static int init_pd(const prb_t *p, dnnl_softmax_desc_t &sd,
        dnnl_primitive_desc_t &spd, res_t *r) {
    dnnl_memory_desc_t data_d;

    const int ndims = (int)p->dims.size();
    DNN_SAFE(dnnl_memory_desc_init_by_tag(
                     &data_d, ndims, p->dims.data(), p->dt, p->tag),
            WARN);

    if (p->dir & FLAG_FWD) {
        auto prop = p->dir & FLAG_INF ? dnnl_forward_inference
                                      : dnnl_forward_training;

        if (p->alg == SOFTMAX)
            DNN_SAFE(
                    dnnl_softmax_forward_desc_init(&sd, prop, &data_d, p->axis),
                    WARN);
        else if (p->alg == LOGSOFTMAX)
            DNN_SAFE(dnnl_logsoftmax_forward_desc_init(
                             &sd, prop, &data_d, p->axis),
                    WARN);
        else
            SAFE_V(FAIL);
    } else {
        dnnl_memory_desc_t diff_data_d;
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_data_d, ndims,
                         p->dims.data(), p->dt, dnnl_format_tag_any),
                WARN);
        if (p->alg == SOFTMAX)
            DNN_SAFE(dnnl_softmax_backward_desc_init(
                             &sd, &diff_data_d, &data_d, p->axis),
                    WARN);
        else if (p->alg == LOGSOFTMAX)
            DNN_SAFE(dnnl_logsoftmax_backward_desc_init(
                             &sd, &diff_data_d, &data_d, p->axis),
                    WARN);
        else
            SAFE_V(FAIL);
    }

    dnnl_status_t init_status
            = dnnl_primitive_desc_create(&spd, &sd, NULL, engine_tgt, NULL);

    if (init_status == dnnl_unimplemented)
        return r->state = UNIMPLEMENTED, OK;
    else
        SAFE(init_status, WARN);

    const char *impl_str = query_impl_info(spd);
    if (maybe_skip(skip_impl, impl_str)) {
        print(2, "SKIPPED: dnnl implementation: %s\n", impl_str);
        DNN_SAFE(dnnl_primitive_desc_destroy(spd), WARN);
        return r->state = SKIPPED, OK;
    } else {
        print(5, "dnnl implementation: %s\n", impl_str);
    }

    return OK;
}

static int compare(const prb_t *p, const dnn_mem_t &fp_mem,
        const dnn_mem_t &dt_mem, res_t *r) {
    const int f32_mant_digits = 24;
    const float trh_coeff_dt = (1 << (f32_mant_digits - digits_dt(p->dt)));
    const float trh_coeff_log = p->alg == LOGSOFTMAX ? 4 : 1;
    const float trh = trh_coeff_dt * trh_coeff_log * 1e-6;

    const auto nelems = dt_mem.nelems();
    r->errors = 0;
    r->total = nelems;

    for (int64_t i = 0; i < nelems; i++) {
        const float dt = dt_mem.get_elem(i);
        const float fp = fp_mem.get_elem(i);

        const float diff = fabsf(fp - dt);
        const float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);
        bool ok = (fabsf(fp) > 1e-5 ? rel_diff : diff) <= trh;

        // check for abs error
        if (!ok) ok = diff < 1e-7;

        r->errors += !ok;

        const bool dump = false || (!ok && (r->errors < 10 || verbose >= 10))
                || (verbose >= 50 && i < 30) || (verbose >= 99);
        if (dump) {
            std::stringstream ss;
            dims_t dims_idx = off2dims_idx(p->dims, i);
            ss << dims_idx;
            std::string ind_str = ss.str();

            print(0, "[%4ld][%s] fp:%8g dt:%8g diff:%8g rdiff:%8g\n", (long)i,
                    ind_str.c_str(), fp, dt, diff, rel_diff);
        }
    }

    if (r->errors) r->state = FAILED;

    if (r->state == UNTESTED) r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

int fill_data_fwd(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp) {
    int64_t outer_size = 0, inner_size = 0, axis_size = 0;
    get_sizes(p, outer_size, inner_size, axis_size);

    // Fill data the way it tests two modes: max_val < 0 and max_val >= 0;
    // Test max_val < 0 by using only negative numbers to check correct max_val
    // subtraction, mostly if library used signed value, not abs.
    // Test max_val >= 0 by exceeding `exp_overflow_arg` value to check answer
    // does not contain +infinity (nan).
    // Distribute several top-1 values to check softmax works right. Also use
    // bit more top-2 values so they contribute in final exp sum as well. Fill
    // much more values with top-3 to check we apply correct maths for whole
    // input.
    // Filling data such way prevents cancellation error for LOGSOFTMAX due to
    // log(sum(x_j)) won't be close to zero as in case of single top-1 value.
    const int exp_overflow_arg = 88;
    const int top1_val = exp_overflow_arg + 2;
    const int top2_val = exp_overflow_arg + 1;
    const int top3_val = exp_overflow_arg;
    const float top1_prob = 4. / axis_size;
    const float top2_prob = 7. * top1_prob;
    const float top3_prob = 3. * top2_prob;

    dnnl::impl::parallel_nd(outer_size, axis_size, inner_size,
            [&](int64_t ou, int64_t as, int64_t in) {
                const int sign = (outer_size > 1 ? ou : in) % 2 == 0 ? -1 : 1;
                const int gen = 13 * ou + 101 * as + 7 * in + 1637;
                const bool top1 = flip_coin(gen, top1_prob);
                const bool top2 = !top1 && flip_coin(gen, top2_prob);
                const bool top3 = !top1 && !top2 && flip_coin(gen, top3_prob);
                const int value = sign
                        * (top1 * top1_val + top2 * top2_val + top3 * top3_val);
                const int64_t ou_in_offset = ou * axis_size * inner_size + in;
                mem_fp.set_elem(ou_in_offset + as * inner_size, value);
            });

    SAFE(mem_dt.reorder(mem_fp), WARN);

    return OK;
}

int fill_data_bwd(
        const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, int seed) {
    const auto nelems = mem_fp.nelems();
    const int range = 128;

    // keep all values negative to have sum and sub of same sign, avoiding
    // cancellation error.
    dnnl::impl::parallel_nd(nelems, [&](int64_t i) {
        const float gen = ((11 * i) + 37 + 19 * seed) % range;
        const float value = -gen / range;
        mem_fp.set_elem(i, value);
    });

    SAFE(mem_dt.reorder(mem_fp), WARN);

    return OK;
}

int doit(const prb_t *p, res_t *r) {
    if (bench_mode == LIST) return r->state = LISTED, OK;

    dnnl_softmax_desc_t sd;
    dnnl_primitive_desc_t spd;
    dnnl_primitive_t s;

    SAFE(init_pd(p, sd, spd, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED) return OK;

    DNN_SAFE(dnnl_primitive_create(&s, spd), WARN);
    DNN_SAFE(dnnl_primitive_desc_destroy(spd), CRIT);

    const auto fp = dnnl_f32;
    const auto tag = get_default_tag((int)p->dims.size());
    auto &data_desc = sd.data_desc;
    dnn_mem_t src_fp(data_desc, fp, tag, engine_tgt);
    dnn_mem_t src_dt(data_desc, engine_tgt);

    dnn_mem_t dst_fp(data_desc, fp, tag, engine_tgt);
    dnn_mem_t placeholder_dst_dt;
    if (!p->inplace) {
        placeholder_dst_dt = dnn_mem_t(data_desc, engine_tgt);
        SAFE(placeholder_dst_dt.reorder(dst_fp), WARN);
    }
    dnn_mem_t &dst_dt = !p->inplace ? placeholder_dst_dt : src_dt;

    dnn_mem_t d_dst_dt, d_src_dt;
    dnn_mem_t d_dst_fp, d_src_fp;

    args_t args;

    if (p->dir & FLAG_FWD) {
        SAFE(fill_data_fwd(p, src_dt, src_fp), WARN);

        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_DST, dst_dt);

        DNN_SAFE(execute_and_wait(s, stream_tgt, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_fwd(p, src_fp, dst_fp);
            dnn_mem_t dst(dst_dt, fp, tag, engine_tgt);
            SAFE(compare(p, dst_fp, dst, r), WARN);
        }
    } else {
        const_dnnl_primitive_desc_t const_spd;
        DNN_SAFE(dnnl_primitive_get_primitive_desc(s, &const_spd), CRIT);
        const auto &d_data_desc = *dnnl_primitive_desc_query_md(
                const_spd, dnnl_query_diff_src_md, 0);

        d_dst_fp = dnn_mem_t(d_data_desc, fp, tag, engine_tgt),
        d_dst_dt = dnn_mem_t(d_data_desc, engine_tgt);

        d_src_fp = dnn_mem_t(d_data_desc, fp, tag, engine_tgt);
        dnn_mem_t placeholder_d_src_dt;
        if (!p->inplace) {
            placeholder_d_src_dt = dnn_mem_t(d_data_desc, engine_tgt);
            SAFE(placeholder_d_src_dt.reorder(d_src_fp), WARN);
        }
        dnn_mem_t &d_src_dt = !p->inplace ? placeholder_d_src_dt : d_dst_dt;

        SAFE(fill_data_bwd(p, src_dt, src_fp, 0), WARN);
        SAFE(fill_data_bwd(p, d_dst_dt, d_dst_fp, 1), WARN);

        args.set(DNNL_ARG_DST, src_dt);
        args.set(DNNL_ARG_DIFF_DST, d_dst_dt);
        args.set(DNNL_ARG_DIFF_SRC, d_src_dt);

        DNN_SAFE(execute_and_wait(s, stream_tgt, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_bwd(p, src_fp, d_dst_fp, d_src_fp);
            dnn_mem_t d_src(d_src_dt, fp, tag, engine_tgt);
            SAFE(compare(p, d_src_fp, d_src, r), WARN);
        }
    }

    measure_perf(r->timer, s, args);

    DNN_SAFE(dnnl_primitive_destroy(s), CRIT);

    return OK;
}

} // namespace softmax
