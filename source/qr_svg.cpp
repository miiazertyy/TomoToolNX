// Minimal QR Code encoder — byte mode, versions 1-10, ECC M
// Enough for http://192.168.x.x:8080 (22 chars → version 1 or 2)
// Based on the QR Code specification (ISO/IEC 18004:2015)
// All lookup tables from the spec.

#include "qr_svg.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace QrSvg {

// ── Reed-Solomon GF(256) ──────────────────────────────────────────────────────

static const uint8_t EXP[512] = {
    1,2,4,8,16,32,64,128,29,58,116,232,205,135,19,38,76,152,45,90,180,117,234,201,143,3,6,12,24,48,96,192,
    157,39,78,156,37,74,148,53,106,212,181,119,238,193,159,35,70,140,5,10,20,40,80,160,93,186,105,210,185,
    111,222,161,95,190,97,194,153,47,94,188,101,202,137,15,30,60,120,240,253,231,211,187,107,214,177,127,254,
    225,223,163,91,182,113,226,217,175,67,134,17,34,68,136,13,26,52,104,208,189,103,206,129,31,62,124,248,
    237,199,147,59,118,236,197,151,51,102,204,133,23,46,92,184,109,218,169,79,158,33,66,132,21,42,84,168,77,
    154,41,82,164,85,170,73,146,57,114,228,213,183,115,230,209,191,99,198,145,63,126,252,229,215,179,123,246,
    241,255,227,219,171,75,150,49,98,196,149,55,110,220,165,87,174,65,130,25,50,100,200,141,7,14,28,56,112,
    224,221,167,83,166,81,162,89,178,121,242,249,239,195,155,43,86,172,69,138,9,18,36,72,144,61,122,244,245,
    247,243,251,235,203,139,11,22,44,88,176,125,250,233,207,131,27,54,108,216,173,71,142,1,
    // repeat for exp table (wrap at 255)
    1,2,4,8,16,32,64,128,29,58,116,232,205,135,19,38,76,152,45,90,180,117,234,201,143,3,6,12,24,48,96,192,
    157,39,78,156,37,74,148,53,106,212,181,119,238,193,159,35,70,140,5,10,20,40,80,160,93,186,105,210,185,
    111,222,161,95,190,97,194,153,47,94,188,101,202,137,15,30,60,120,240,253,231,211,187,107,214,177,127,254,
    225,223,163,91,182,113,226,217,175,67,134,17,34,68,136,13,26,52,104,208,189,103,206,129,31,62,124,248,
    237,199,147,59,118,236,197,151,51,102,204,133,23,46,92,184,109,218,169,79,158,33,66,132,21,42,84,168,77,
    154,41,82,164,85,170,73,146,57,114,228,213,183,115,230,209,191,99,198,145,63,126,252,229,215,179,123,246,
    241,255,227,219,171,75,150,49,98,196,149,55,110,220,165,87,174,65,130,25,50,100,200,141,7,14,28,56,112,
    224,221,167,83,166,81,162,89,178,121,242,249,239,195,155,43,86,172,69,138,9,18,36,72,144,61,122,244,245,
    247,243,251,235,203,139,11,22,44,88,176,125,250,233,207,131,27,54,108,216,173,71,142
};

static uint8_t LOG[256];
static bool logsReady = false;
static void buildLog() {
    if (logsReady) return;
    logsReady = true;
    for (int i = 0; i < 255; i++) LOG[EXP[i]] = (uint8_t)i;
}

static uint8_t gmul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return EXP[(LOG[a] + LOG[b]) % 255];
}

static std::vector<uint8_t> rsEncode(const std::vector<uint8_t>& data, int nec) {
    buildLog();
    // Generator polynomial coefficients
    std::vector<uint8_t> gen(nec + 1, 0);
    gen[0] = 1;
    for (int i = 0; i < nec; i++) {
        uint8_t a = EXP[i];
        for (int j = i + 1; j > 0; j--)
            gen[j] = gen[j-1] ^ gmul(gen[j], a);
        gen[0] = gmul(gen[0], a);
    }
    std::vector<uint8_t> rem(nec, 0);
    for (uint8_t b : data) {
        uint8_t coef = b ^ rem[0];
        for (int i = 0; i < nec - 1; i++)
            rem[i] = rem[i+1] ^ gmul(gen[i+1], coef);
        rem[nec-1] = gmul(gen[nec], coef);
    }
    return rem;
}

// ── QR tables (ECC M) ─────────────────────────────────────────────────────────
// version: total data bytes, ec bytes per block, blocks
struct VerInfo { int dataBits; int ecPerBlock; int b1; int b2; int c2; };
// ECC M
static const VerInfo VERS[] = {
    {0},          // 0 unused
    {128,10,1,0,0},   // v1: 16 data bytes, 10 ec
    {224,16,1,0,0},   // v2: 28 data bytes, 16 ec
    {352,26,2,0,0},   // v3: 44 data bytes, 26 ec
    {512,18,2,0,0},   // v4: 64 data bytes, 18 ec (2 blocks of 32 each? no)
    // simplified: just use enough for our URL
};
// Actual data codewords for ECC M per version
static const int DATA_CODEWORDS_M[] = {0,16,28,44,64,86,108,124,154,182,216};
static const int EC_CODEWORDS_M[]   = {0,10,16,26,18,24,16,18,22,22,26};
static const int BLOCKS_M[]         = {0,1,1,1,2,2,4,4,4,5,5};

// ── Bit buffer ────────────────────────────────────────────────────────────────
struct BitBuf {
    std::vector<uint8_t> bytes;
    int bitLen = 0;
    void appendBits(uint32_t val, int n) {
        for (int i = n-1; i >= 0; i--) {
            if (bitLen % 8 == 0) bytes.push_back(0);
            if ((val >> i) & 1) bytes[bitLen/8] |= 0x80 >> (bitLen % 8);
            bitLen++;
        }
    }
};

// ── QR matrix ─────────────────────────────────────────────────────────────────
struct QR {
    int N;
    std::vector<uint8_t> mod;  // 0=light, 1=dark, 2=reserved
    QR(int ver) : N(ver*4+17), mod((ver*4+17)*(ver*4+17), 0) {}
    bool& at(int r, int c) { return (bool&)mod[r*N+c]; }
    bool get(int r, int c) const { return mod[r*N+c] & 1; }
    void set(int r, int c, bool dark) { mod[r*N+c] = dark ? 1 : 0; }
    void setF(int r, int c, bool dark) { mod[r*N+c] = dark ? 3 : 2; } // function
    bool isFunc(int r, int c) const { return mod[r*N+c] >= 2; }
};

static void drawFinder(QR& qr, int r, int c) {
    for (int dr=-4; dr<=4; dr++) for (int dc=-4; dc<=4; dc++) {
        int rr=r+dr, cc=c+dc;
        if (rr<0||rr>=qr.N||cc<0||cc>=qr.N) continue;
        bool d = std::max(std::abs(dr),std::abs(dc)) != 3;
        if (std::abs(dr)<=3 && std::abs(dc)<=3)
            qr.setF(rr,cc,d);
    }
}

static void drawAlignment(QR& qr, int r, int c) {
    for (int dr=-2; dr<=2; dr++) for (int dc=-2; dc<=2; dc++) {
        bool d = std::max(std::abs(dr),std::abs(dc)) != 1;
        qr.setF(r+dr,c+dc,d);
    }
}

// Alignment pattern positions per version
static const std::vector<int> alignPos(int ver) {
    if (ver == 1) return {};
    if (ver == 2) return {6,18};
    if (ver == 3) return {6,22};
    if (ver == 4) return {6,26};
    if (ver == 5) return {6,30};
    if (ver == 6) return {6,34};
    if (ver == 7) return {6,22,38};
    return {6,24,42};
}

static void drawTimingAndFormat(QR& qr, int ver, int mask) {
    // Timing patterns
    for (int i=8; i<qr.N-8; i++) {
        qr.setF(6,i,i%2==0);
        qr.setF(i,6,i%2==0);
    }
    // Dark module
    qr.setF(qr.N-8,8,true);

    // Format info: ECC=M(01), mask
    static const int formatInfo[8] = {
        0x77C4,0x72F3,0x7DAA,0x789D,0x662F,0x6318,0x6C41,0x6976
    };
    int fmt = formatInfo[mask];
    for (int i=0; i<6; i++) {
        bool b=(fmt>>i)&1;
        qr.setF(8,i,b); qr.setF(i,8,b);
    }
    qr.setF(8,7,(fmt>>6)&1); qr.setF(7,8,(fmt>>6)&1);
    qr.setF(8,8,(fmt>>7)&1); qr.setF(8,8,(fmt>>7)&1);
    for (int i=8; i<15; i++) {
        bool b=(fmt>>i)&1;
        qr.setF(8,qr.N-15+i,b);
        qr.setF(qr.N-15+i,8,b);
    }
}

static void placeData(QR& qr, const std::vector<uint8_t>& data) {
    int i=0;
    for (int right=qr.N-1; right>=1; right-=2) {
        if (right==6) right=5;
        for (int vert=0; vert<qr.N; vert++) {
            for (int j=0; j<2; j++) {
                int c=right-j;
                int r=(right<6||right%2==0)?vert:(qr.N-1-vert);
                if (!qr.isFunc(r,c)) {
                    bool bit=false;
                    if (i<(int)data.size()*8) bit=(data[i/8]>>(7-i%8))&1;
                    qr.set(r,c,bit); i++;
                }
            }
        }
    }
}

static void applyMask(QR& qr, int mask) {
    for (int r=0; r<qr.N; r++) for (int c=0; c<qr.N; c++) {
        if (qr.isFunc(r,c)) continue;
        bool inv=false;
        switch(mask) {
            case 0: inv=(r+c)%2==0; break;
            case 1: inv=r%2==0; break;
            case 2: inv=c%3==0; break;
            case 3: inv=(r+c)%3==0; break;
            case 4: inv=(r/2+c/3)%2==0; break;
            case 5: inv=(r*c)%2+(r*c)%3==0; break;
            case 6: inv=((r*c)%2+(r*c)%3)%2==0; break;
            case 7: inv=((r+c)%2+(r*c)%3)%2==0; break;
        }
        if (inv) qr.set(r,c,!qr.get(r,c));
    }
}

static int penalty(const QR& qr) {
    int score=0;
    // Rule 1: 5+ in a row
    for (int r=0; r<qr.N; r++) {
        int runC=1;
        for (int c=1; c<qr.N; c++) {
            if (qr.get(r,c)==qr.get(r,c-1)) { if(++runC==5)score+=3; else if(runC>5)score++; }
            else runC=1;
        }
    }
    for (int c=0; c<qr.N; c++) {
        int runR=1;
        for (int r=1; r<qr.N; r++) {
            if (qr.get(r,c)==qr.get(r-1,c)) { if(++runR==5)score+=3; else if(runR>5)score++; }
            else runR=1;
        }
    }
    return score;
}

std::string Generate(const std::string& url, int moduleSize, int border) {
    // Choose version
    int ver=1;
    for (; ver<=10; ver++) {
        if ((int)url.size() <= DATA_CODEWORDS_M[ver]-3) break; // 4 mode+len + data + term
    }
    if (ver>10) return "";

    int ndata = DATA_CODEWORDS_M[ver];
    int nec   = EC_CODEWORDS_M[ver];
    int nblocks = BLOCKS_M[ver];

    // Encode: mode indicator (byte=0100, 4bits) + length (8bits) + data
    BitBuf bb;
    bb.appendBits(4, 4); // byte mode
    bb.appendBits((uint32_t)url.size(), 8);
    for (char ch : url) bb.appendBits((uint8_t)ch, 8);
    bb.appendBits(0, 4); // terminator
    while (bb.bytes.size() < (size_t)ndata) {
        bb.bytes.push_back(0xEC);
        if ((int)bb.bytes.size() < ndata) bb.bytes.push_back(0x11);
    }
    bb.bytes.resize(ndata);

    // Split into blocks and add EC
    std::vector<uint8_t> allData, allEc;
    int blockSize = ndata / nblocks;
    int blockEcSize = nec / nblocks;
    for (int b=0; b<nblocks; b++) {
        std::vector<uint8_t> block(bb.bytes.begin()+b*blockSize, bb.bytes.begin()+(b+1)*blockSize);
        auto ec = rsEncode(block, blockEcSize);
        for (auto x:block) allData.push_back(x);
        for (auto x:ec) allEc.push_back(x);
    }
    std::vector<uint8_t> codewords;
    // Interleave (for single block just concat)
    if (nblocks==1) { for(auto x:allData)codewords.push_back(x); for(auto x:allEc)codewords.push_back(x); }
    else {
        for (int i=0; i<blockSize; i++) for (int b=0; b<nblocks; b++) codewords.push_back(allData[b*blockSize+i]);
        for (int i=0; i<blockEcSize; i++) for (int b=0; b<nblocks; b++) codewords.push_back(allEc[b*blockEcSize+i]);
    }

    // Build matrix
    QR qr(ver);
    drawFinder(qr,3,3); drawFinder(qr,3,qr.N-4); drawFinder(qr,qr.N-4,3);
    auto ap = alignPos(ver);
    if (ap.size()>=2) for (int r:ap) for (int c:ap) {
        if ((r==6&&c==6)||(r==6&&c==ap.back())||(r==ap.back()&&c==6)) continue;
        drawAlignment(qr,r,c);
    }
    // Try each mask, pick best
    int bestMask=0, bestPenalty=INT32_MAX;
    for (int m=0; m<8; m++) {
        QR tmp=qr;
        drawTimingAndFormat(tmp,ver,m);
        placeData(tmp,codewords);
        applyMask(tmp,m);
        int p=penalty(tmp);
        if (p<bestPenalty) { bestPenalty=p; bestMask=m; }
    }
    drawTimingAndFormat(qr,ver,bestMask);
    placeData(qr,codewords);
    applyMask(qr,bestMask);

    // Render SVG
    int total = qr.N*moduleSize + border*2*moduleSize;
    char buf[256];
    snprintf(buf,sizeof(buf),
        "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d'>",
        total,total);
    std::string svg=buf;
    snprintf(buf,sizeof(buf),
        "<rect width='%d' height='%d' fill='white'/>",total,total);
    svg+=buf;
    for (int r=0; r<qr.N; r++) for (int c=0; c<qr.N; c++) {
        if (qr.get(r,c)) {
            snprintf(buf,sizeof(buf),
                "<rect x='%d' y='%d' width='%d' height='%d' fill='black'/>",
                (c+border)*moduleSize,(r+border)*moduleSize,moduleSize,moduleSize);
            svg+=buf;
        }
    }
    svg+="</svg>";
    return svg;
}

} // namespace QrSvg
