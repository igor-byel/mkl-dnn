# Attributes

## Usage
```
    [--attr="attr_str"]
```

The attribute string *attr_str* is defined as follows (line breaks are for
readability):
```
    [oscale={none,common,per_oc}[:scale];]
    [post_ops='eltwise[:alpha[:beta[:int8_eltwise_scale]]];sum[:sum_scale];';]
```

where `oscale` stands for output_scales. The first parameter is the policy that
is defined below. `scale` is the second optional parameter, which is a real
number that specifies either the one common output scale (for the `none` and
`common` polices) or a starting point for the `per_oc` policy, which uses many
scales. The default scale is `1.0`.

Known policies are:
  - `none` (default) means no output scales set (i.e. scale = 1)
  - `common` corresponds to `mask=0` with common scale factor
  - `per_oc` corresponds to `mask=1<<1` (i.e. output channels) with different
     scale factors
  - `per_dim_0`  corresponds to `mask=1<<0`. Elements corresponding to a single
                 dim0 point have same factor. Different dim0 points have
                 different scaling factors.
  - `per_dim_1`  corresponds to `mask=1<<1`. Elements corresponding to a single
                 dim1 point have same factor. Different dim1 points have
                 different scaling factors.
  - `per_dim_01` corresponds to `mask=(1<<0) + (1<<1)`. Elements corresponding
                 to a single {dim0, dim1} point have same factor. Different
                 {dim0, dim1} points have different scaling factors.

`post_ops` stands for post operation sequence. All post operations support
output scale (relevant for int8 operations only) which is used as a multiplier
before storing the result. Some post operations support custom alpha and beta
constants with a default value of 0. Operations may be called in any order, e.g.
apply `sum` at first and then apply `relu`, or vice versa - apply `relu` and
then `sum` it with destination. Up to 4 post operations applied is supported.

Currently supported post operations:
  - `sum` -- appends operation result to the output.

Eltwise operations that support no alpha or beta:
  - `abs`
  - `exp`
  - `gelu`
  - `log`
  - `logistic`
  - `sqrt`
  - `square`
  - `srelu`
  - `tanh`

Eltwise operations that support only alpha:
  - `brelu`
  - `elu`
  - `relu`

Eltwise operations that support both alpha and beta:
  - `linear`


## Examples:

Run a set of f32 forward convolutions without bias appending accumulation into
destination and perform relu on the output with scale set to 0.5:
``` sh
    ./benchdnn --conv --cfg=f32 --dir=FWD_D \
               --attr=post_ops='sum;relu:0.5' --batch=conv_tails
```

Run a 1D-spatial reorder problem with s8 input data and u8 output data in four
different physical memory layout combinations {ncw, ncw}, {ncw, nwc},
{nwc, ncw} and {nwc, nwc} applying output scale 2.5 for each output point:
``` sh
    ./benchdnn --reorder --sdt=s8 --ddt=u8 \
               --stag=ncw,nwc --dtag=ncw,nwc \
               --attr=oscale=common:2.5 2x8x8
```
