# rgbcx

BC1 encoder by Richard Geldreich, Jr., from
[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) (`rgbcx.h`, `rgbcx.cpp`,
`rgbcx_table4_small.h`, v1.13), unchanged. Public domain or MIT, as you choose:
the full text is at the end of `rgbcx.h`.

TomoToolNX builds it through `source/rgbcx_impl.cpp` with the smaller tables
(`RGBCX_USE_SMALLER_TABLES 1`) and uses it for the "rgbcx" picture encoder.
