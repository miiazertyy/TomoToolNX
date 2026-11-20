// The rgbcx BC1 encoder (libs/rgbcx, public domain or MIT), built here once,
// with the smaller lookup tables to keep the app small.
#define RGBCX_USE_SMALLER_TABLES 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../libs/rgbcx/rgbcx.cpp"
#pragma GCC diagnostic pop
