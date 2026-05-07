// u2net_infer.cpp
// u2netp background removal — minimal ONNX-derived graph executor.
// Reads a preprocessed binary produced by tools/preprocess_u2net.py.
// No exceptions (-fno-exceptions compatible).

#include "u2net_infer.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>

namespace U2Net {

// ─── NCHW float tensor ────────────────────────────────────────────────────────

struct Tensor {
    int n = 0, c = 0, h = 0, w = 0;
    std::vector<float> d;

    Tensor() = default;
    Tensor(int n_, int c_, int h_, int w_)
        : n(n_), c(c_), h(h_), w(w_), d((size_t)n_ * c_ * h_ * w_, 0.f) {}

    float& at(int _n, int _c, int _h, int _w) {
        return d[(size_t)_n * (c * h * w) + (size_t)_c * (h * w) + _h * w + _w];
    }
    float at(int _n, int _c, int _h, int _w) const {
        return d[(size_t)_n * (c * h * w) + (size_t)_c * (h * w) + _h * w + _w];
    }
    size_t size() const { return (size_t)n * c * h * w; }
};

// ─── Binary format reader ─────────────────────────────────────────────────────

struct BinReader {
    const uint8_t* data;
    size_t         pos;
    size_t         len;
    bool           err = false;

    uint32_t read_u32() {
        if (pos + 4 > len) { err = true; return 0; }
        uint32_t v;
        memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    }
    int32_t read_i32() { return (int32_t)read_u32(); }
    uint8_t read_u8() {
        if (pos >= len) { err = true; return 0; }
        return data[pos++];
    }
    std::string read_str() {
        uint32_t n = read_u32();
        if (pos + n > len) { err = true; return {}; }
        std::string s((const char*)(data + pos), n);
        pos += n;
        return s;
    }
    bool read_floats(std::vector<float>& v, size_t count) {
        size_t bytes = count * 4;
        if (pos + bytes > len) { err = true; return false; }
        v.resize(count);
        memcpy(v.data(), data + pos, bytes);
        pos += bytes;
        return true;
    }
};

// ─── Op codes (must match preprocess_u2net.py) ───────────────────────────────

enum OpCode : uint8_t {
    OP_CONV    = 0,
    OP_RELU    = 1,
    OP_MAXPOOL = 2,
    OP_CONCAT  = 3,
    OP_RESIZE  = 4,
    OP_SIGMOID = 5,
    OP_ADD     = 6,
};

struct Node {
    OpCode              op;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    int32_t             attrs[6] = {};
};

// ─── Weight / value map ───────────────────────────────────────────────────────

struct WeightEntry {
    std::vector<int32_t> shape;
    std::vector<float>   data;
};
using WeightMap = std::unordered_map<std::string, WeightEntry>;
using ValMap    = std::unordered_map<std::string, Tensor>;

// ─── Load binary ──────────────────────────────────────────────────────────────

static bool LoadBin(const char* path,
                    WeightMap&       weights,
                    std::vector<Node>& nodes,
                    std::string&     err_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) { err_out = std::string("cannot open ") + path; return false; }

    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> buf((size_t)flen);
    if (fread(buf.data(), 1, (size_t)flen, f) != (size_t)flen) {
        fclose(f); err_out = "read error"; return false;
    }
    fclose(f);

    BinReader r{buf.data(), 0, (size_t)flen};

    // magic
    uint32_t magic = r.read_u32();
    uint32_t ver   = r.read_u32();
    if (magic != 0x504E3255u || ver != 1) {   // 'U2NP' little-endian
        err_out = "bad magic/version in u2netp.bin"; return false;
    }

    // weights
    uint32_t nw = r.read_u32();
    weights.reserve(nw);
    for (uint32_t i = 0; i < nw; i++) {
        std::string name = r.read_str();
        WeightEntry we;
        uint32_t ndim = r.read_u32();
        we.shape.resize(ndim);
        for (uint32_t d = 0; d < ndim; d++) we.shape[d] = r.read_i32();
        uint32_t count = r.read_u32();
        if (!r.read_floats(we.data, count)) {
            err_out = "truncated weight data"; return false;
        }
        weights[name] = std::move(we);
    }

    // nodes
    uint32_t nn = r.read_u32();
    nodes.resize(nn);
    for (uint32_t i = 0; i < nn; i++) {
        Node& nd  = nodes[i];
        nd.op     = (OpCode)r.read_u8();
        uint8_t ni = r.read_u8();
        nd.inputs.resize(ni);
        for (uint8_t k = 0; k < ni; k++) nd.inputs[k] = r.read_str();
        uint8_t no = r.read_u8();
        nd.outputs.resize(no);
        for (uint8_t k = 0; k < no; k++) nd.outputs[k] = r.read_str();
        for (int k = 0; k < 6; k++) nd.attrs[k] = r.read_i32();
    }

    if (r.err) { err_out = "binary truncated"; return false; }
    return true;
}

// ─── Neural network primitives ────────────────────────────────────────────────

// Inline ReLU on tensor
static void ApplyRelu(Tensor& t) {
    for (float& v : t.d) if (v < 0.f) v = 0.f;
}

// Inline Sigmoid on tensor
static void ApplySigmoid(Tensor& t) {
    for (float& v : t.d) v = 1.f / (1.f + expf(-v));
}

// Element-wise Add (a += b)
static void ApplyAdd(Tensor& a, const Tensor& b) {
    for (size_t i = 0; i < a.d.size(); i++) a.d[i] += b.d[i];
}

// Concat along channel axis (axis=1, N must match)
static Tensor ConcatC(const std::vector<const Tensor*>& parts) {
    int n = parts[0]->n, h = parts[0]->h, w = parts[0]->w;
    int c_total = 0;
    for (auto* p : parts) c_total += p->c;
    Tensor out(n, c_total, h, w);
    int c_off = 0;
    for (auto* p : parts) {
        for (int bn = 0; bn < n; bn++)
            for (int bc = 0; bc < p->c; bc++) {
                size_t src = (size_t)bn * p->c * h * w + (size_t)bc * h * w;
                size_t dst = (size_t)bn * c_total * h * w + (size_t)(c_off + bc) * h * w;
                memcpy(out.d.data() + dst, p->d.data() + src, (size_t)h * w * sizeof(float));
            }
        c_off += p->c;
    }
    return out;
}

// Bilinear upsample (pytorch_half_pixel coordinate mode)
static Tensor Upsample(const Tensor& src, int th, int tw) {
    Tensor out(src.n, src.c, th, tw);
    for (int bn = 0; bn < src.n; bn++) {
        for (int bc = 0; bc < src.c; bc++) {
            const float* sp = src.d.data() + ((size_t)bn * src.c + bc) * src.h * src.w;
            float*       dp = out.d.data() + ((size_t)bn * src.c + bc) * th * tw;
            for (int y = 0; y < th; y++) {
                float fy = (y + 0.5f) * (float)src.h / th - 0.5f;
                if (fy < 0.f) fy = 0.f;
                int y0 = (int)fy, y1 = y0 + 1;
                if (y1 >= src.h) y1 = src.h - 1;
                float wy1 = fy - y0, wy0 = 1.f - wy1;
                const float* rp0 = sp + y0 * src.w;
                const float* rp1 = sp + y1 * src.w;
                float* outr = dp + y * tw;
                for (int x = 0; x < tw; x++) {
                    float fx = (x + 0.5f) * (float)src.w / tw - 0.5f;
                    if (fx < 0.f) fx = 0.f;
                    int x0 = (int)fx, x1 = x0 + 1;
                    if (x1 >= src.w) x1 = src.w - 1;
                    float wx1 = fx - x0, wx0 = 1.f - wx1;
                    outr[x] = wy0 * (wx0 * rp0[x0] + wx1 * rp0[x1])
                            + wy1 * (wx0 * rp1[x0] + wx1 * rp1[x1]);
                }
            }
        }
    }
    return out;
}

// MaxPool 2D with optional ceil_mode
static Tensor MaxPool2D(const Tensor& src, int kh, int kw, int sh, int sw,
                         int ph, int pw, bool ceil_mode)
{
    auto out_dim = [&](int in, int k, int s, int p) -> int {
        int raw = in + 2 * p - k;
        return ceil_mode ? (raw + s - 1) / s + 1 : raw / s + 1;
    };
    int oh = out_dim(src.h, kh, sh, ph);
    int ow = out_dim(src.w, kw, sw, pw);
    Tensor out(src.n, src.c, oh, ow);
    for (int bn = 0; bn < src.n; bn++)
        for (int bc = 0; bc < src.c; bc++) {
            const float* sp = src.d.data() + ((size_t)bn * src.c + bc) * src.h * src.w;
            float*       dp = out.d.data() + ((size_t)bn * src.c + bc) * oh * ow;
            for (int y = 0; y < oh; y++) {
                int iy0 = y * sh - ph;
                for (int x = 0; x < ow; x++) {
                    int ix0 = x * sw - pw;
                    float mx = -1e30f;
                    for (int ky = 0; ky < kh; ky++) {
                        int iy = iy0 + ky;
                        if (iy < 0 || iy >= src.h) continue;
                        for (int kx = 0; kx < kw; kx++) {
                            int ix = ix0 + kx;
                            if (ix >= 0 && ix < src.w)
                                mx = std::max(mx, sp[iy * src.w + ix]);
                        }
                    }
                    dp[y * ow + x] = (mx > -1e29f) ? mx : 0.f;
                }
            }
        }
    return out;
}

// 2D Convolution (NCHW weights [OC,IC,KH,KW])
static Tensor Conv2D(const Tensor& src,
                     const WeightEntry& w_e, const WeightEntry& b_e,
                     int pad_h, int pad_w, int str_h, int str_w,
                     int dil_h, int dil_w)
{
    int oc  = w_e.shape[0];
    int ic  = w_e.shape[1];
    int kh  = w_e.shape[2];
    int kw  = w_e.shape[3];
    int oh  = (src.h + 2 * pad_h - dil_h * (kh - 1) - 1) / str_h + 1;
    int ow  = (src.w + 2 * pad_w - dil_w * (kw - 1) - 1) / str_w + 1;
    int in_h = src.h, in_w = src.w;

    Tensor out(src.n, oc, oh, ow);
    const float* wdata = w_e.data.data();  // [oc, ic, kh, kw]
    const float* bdata = b_e.data.data();  // [oc]

    for (int bn = 0; bn < src.n; bn++) {
        // pre-fill bias
        for (int o = 0; o < oc; o++) {
            float bv = bdata[o];
            float* op = out.d.data() + ((size_t)bn * oc + o) * oh * ow;
            for (int i = 0; i < oh * ow; i++) op[i] = bv;
        }

        // accumulate over input channels and kernel positions
        for (int o = 0; o < oc; o++) {
            float* op = out.d.data() + ((size_t)bn * oc + o) * oh * ow;
            for (int ic_ = 0; ic_ < ic; ic_++) {
                const float* inp = src.d.data() + ((size_t)bn * src.c + ic_) * in_h * in_w;
                const float* wp  = wdata + ((size_t)o * ic + ic_) * kh * kw;
                for (int ky = 0; ky < kh; ky++) {
                    int iy_base = ky * dil_h - pad_h;
                    for (int kx = 0; kx < kw; kx++) {
                        float wv = wp[ky * kw + kx];
                        int ix_base = kx * dil_w - pad_w;
                        for (int y = 0; y < oh; y++) {
                            int iy = y * str_h + iy_base;
                            if (iy < 0 || iy >= in_h) continue;
                            const float* inp_row = inp + iy * in_w;
                            float* out_row = op + y * ow;
                            if (str_w == 1 && dil_w == 1 && pad_w >= 0) {
                                // hot path: stride-1, unit-dilation — compiler can vectorize
                                int x0 = -ix_base, x1 = in_w - ix_base;
                                int ow_start = std::max(0, x0);
                                int ow_end   = std::min(ow, x1);
                                for (int x = ow_start; x < ow_end; x++)
                                    out_row[x] += wv * inp_row[x + ix_base];
                            } else {
                                for (int x = 0; x < ow; x++) {
                                    int ix = x * str_w + ix_base;
                                    if (ix >= 0 && ix < in_w)
                                        out_row[x] += wv * inp_row[ix];
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return out;
}

// ─── Graph executor ───────────────────────────────────────────────────────────

static const char* kGraphOutput = "1959"; // outconv sigmoid (fused output)

static bool RunGraph(const WeightMap&        weights,
                     const std::vector<Node>& nodes,
                     const Tensor&            input,
                     Tensor&                  output,
                     std::string&             err_out,
                     ProgressCb               cb  = nullptr,
                     void*                    ud  = nullptr)
{
    int total = (int)nodes.size();
    int done  = 0;
    ValMap vals;
    vals["input.1"] = input;

    // Helper: get tensor from either val map or weights (as 4D)
    auto get_tensor = [&](const std::string& name, Tensor& out) -> bool {
        auto it = vals.find(name);
        if (it != vals.end()) { out = it->second; return true; }
        auto wi = weights.find(name);
        if (wi != weights.end()) {
            const auto& we = wi->second;
            int sn = we.shape.size() > 0 ? we.shape[0] : 1;
            int sc = we.shape.size() > 1 ? we.shape[1] : 1;
            int sh = we.shape.size() > 2 ? we.shape[2] : 1;
            int sw = we.shape.size() > 3 ? we.shape[3] : 1;
            out.n = sn; out.c = sc; out.h = sh; out.w = sw;
            out.d = we.data;
            return true;
        }
        err_out = std::string("missing tensor: ") + name;
        return false;
    };

    for (const Node& nd : nodes) {
        switch (nd.op) {
        case OP_RELU: {
            Tensor t; if (!get_tensor(nd.inputs[0], t)) return false;
            ApplyRelu(t);
            vals[nd.outputs[0]] = std::move(t);
            break;
        }
        case OP_SIGMOID: {
            Tensor t; if (!get_tensor(nd.inputs[0], t)) return false;
            ApplySigmoid(t);
            vals[nd.outputs[0]] = std::move(t);
            break;
        }
        case OP_ADD: {
            Tensor a, b;
            if (!get_tensor(nd.inputs[0], a)) return false;
            if (!get_tensor(nd.inputs[1], b)) return false;
            ApplyAdd(a, b);
            vals[nd.outputs[0]] = std::move(a);
            break;
        }
        case OP_CONCAT: {
            std::vector<Tensor> parts(nd.inputs.size());
            for (size_t i = 0; i < nd.inputs.size(); i++)
                if (!get_tensor(nd.inputs[i], parts[i])) return false;
            std::vector<const Tensor*> ptrs(parts.size());
            for (size_t i = 0; i < parts.size(); i++) ptrs[i] = &parts[i];
            vals[nd.outputs[0]] = ConcatC(ptrs);
            break;
        }
        case OP_MAXPOOL: {
            Tensor t; if (!get_tensor(nd.inputs[0], t)) return false;
            vals[nd.outputs[0]] = MaxPool2D(t,
                nd.attrs[0], nd.attrs[1], nd.attrs[2], nd.attrs[3],
                0, 0, nd.attrs[4] != 0);
            break;
        }
        case OP_RESIZE: {
            Tensor t; if (!get_tensor(nd.inputs[0], t)) return false;
            vals[nd.outputs[0]] = Upsample(t, nd.attrs[0], nd.attrs[1]);
            break;
        }
        case OP_CONV: {
            auto wi = weights.find(nd.inputs[1]);
            auto bi = weights.find(nd.inputs[2]);
            if (wi == weights.end() || bi == weights.end()) {
                err_out = "missing conv weight/bias: " + nd.inputs[1];
                return false;
            }
            Tensor t; if (!get_tensor(nd.inputs[0], t)) return false;
            vals[nd.outputs[0]] = Conv2D(t, wi->second, bi->second,
                nd.attrs[0], nd.attrs[1], nd.attrs[2], nd.attrs[3],
                nd.attrs[4], nd.attrs[5]);
            break;
        }
        }

        if (cb) cb(++done, total, ud);
    }

    auto it = vals.find(kGraphOutput);
    if (it == vals.end()) { err_out = "output tensor not found"; return false; }
    output = std::move(it->second);
    return true;
}

// ─── Preprocessing: RGBA → normalised 1×3×320×320 ───────────────────────────

static Tensor RgbaToInput(const RgbaImage& img) {
    // resize to 320×320 using bilinear, then normalise with ImageNet mean/std
    const int SIZE = 320;
    Tensor t(1, 3, SIZE, SIZE);

    float* rp = t.d.data();
    float* gp = rp + SIZE * SIZE;
    float* bp = gp + SIZE * SIZE;

    const float mean_r = 0.485f, mean_g = 0.456f, mean_b = 0.406f;
    const float std_r  = 0.229f, std_g  = 0.224f, std_b  = 0.225f;

    for (int y = 0; y < SIZE; y++) {
        float fy = (y + 0.5f) * (float)img.height / SIZE - 0.5f;
        if (fy < 0.f) fy = 0.f;
        int y0 = (int)fy, y1 = y0 + 1;
        if (y1 >= img.height) y1 = img.height - 1;
        float wy1 = fy - y0, wy0 = 1.f - wy1;
        for (int x = 0; x < SIZE; x++) {
            float fx = (x + 0.5f) * (float)img.width / SIZE - 0.5f;
            if (fx < 0.f) fx = 0.f;
            int x0 = (int)fx, x1 = x0 + 1;
            if (x1 >= img.width) x1 = img.width - 1;
            float wx1 = fx - x0, wx0 = 1.f - wx1;

            auto pix = [&](int py, int px) -> const uint8_t* {
                return img.pixels.data() + (py * img.width + px) * 4;
            };
            const uint8_t* p00 = pix(y0, x0);
            const uint8_t* p01 = pix(y0, x1);
            const uint8_t* p10 = pix(y1, x0);
            const uint8_t* p11 = pix(y1, x1);

            float r = (wy0 * (wx0 * p00[0] + wx1 * p01[0])
                     + wy1 * (wx0 * p10[0] + wx1 * p11[0])) / 255.f;
            float g = (wy0 * (wx0 * p00[1] + wx1 * p01[1])
                     + wy1 * (wx0 * p10[1] + wx1 * p11[1])) / 255.f;
            float b = (wy0 * (wx0 * p00[2] + wx1 * p01[2])
                     + wy1 * (wx0 * p10[2] + wx1 * p11[2])) / 255.f;

            rp[y * SIZE + x] = (r - mean_r) / std_r;
            gp[y * SIZE + x] = (g - mean_g) / std_g;
            bp[y * SIZE + x] = (b - mean_b) / std_b;
        }
    }
    return t;
}

// ─── Postprocessing: apply 320×320 mask to original image ────────────────────

static void ApplyMask(RgbaImage& img, const Tensor& mask) {
    // mask is 1×1×320×320, values 0–1 (foreground=1, background=0)
    const float* mp = mask.d.data();
    int mw = mask.w, mh = mask.h;
    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            float fy = (y + 0.5f) * (float)mh / img.height - 0.5f;
            if (fy < 0.f) fy = 0.f;
            int y0 = (int)fy, y1 = y0 + 1;
            if (y1 >= mh) y1 = mh - 1;
            float wy1 = fy - y0, wy0 = 1.f - wy1;
            float fx = (x + 0.5f) * (float)mw / img.width - 0.5f;
            if (fx < 0.f) fx = 0.f;
            int x0 = (int)fx, x1 = x0 + 1;
            if (x1 >= mw) x1 = mw - 1;
            float wx1 = fx - x0, wx0 = 1.f - wx1;
            float v = wy0 * (wx0 * mp[y0 * mw + x0] + wx1 * mp[y0 * mw + x1])
                    + wy1 * (wx0 * mp[y1 * mw + x0] + wx1 * mp[y1 * mw + x1]);
            img.pixels[(y * img.width + x) * 4 + 3] = (uint8_t)(v * 255.f + 0.5f);
        }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

std::string RemoveBackground(RgbaImage& img, const char* model_path,
                              ProgressCb cb, void* userdata) {
    WeightMap        weights;
    std::vector<Node> nodes;
    std::string err;

    if (!LoadBin(model_path, weights, nodes, err))
        return err;

    Tensor input  = RgbaToInput(img);
    Tensor output;
    if (!RunGraph(weights, nodes, input, output, err, cb, userdata))
        return err;

    ApplyMask(img, output);
    return "";
}

} // namespace U2Net
