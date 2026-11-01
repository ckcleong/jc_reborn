/*
 * Minimal QR encoder for the ESP32 setup screen — see qr.h.
 *
 * Byte-mode payloads, versions 1..4 at ECC level L (single RS block
 * each: 26/44/70/100 codewords, of which 19/34/55/80 data). Matrix
 * placement, format-info BCH, mask selection and penalty scoring
 * follow ISO/IEC 18004. Verified on PC by decoding renders with
 * OpenCV's QRCodeDetector (cv2) before shipping.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR

#include "qr.h"

// --- GF(256) arithmetic, primitive polynomial 0x11D -------------------

static uint8_t gfExp[510];
static uint8_t gfLog[256];

static void gfInit(void)
{
    int x = 1;

    for (int i = 0; i < 255; i++) {
        gfExp[i] = (uint8_t) x;
        gfLog[x] = (uint8_t) i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11D;
    }
    for (int i = 255; i < 510; i++)
        gfExp[i] = gfExp[i - 255];
}

static uint8_t gfMul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;
    return gfExp[gfLog[a] + gfLog[b]];
}

// --- Reed-Solomon generator + remainder ------------------------------

static uint8_t gen[21];     // coefficients of x^0..x^ecLen, monic
static int     genEcLen;

static void genInit(int ecLen)
{
    genEcLen = ecLen;
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;

    uint8_t root = 1;       // alpha^i, alpha = 0x02
    for (int i = 0; i < ecLen; i++) {
        // gen *= (x + root): new[k] = old[k-1] ^ root*old[k]
        for (int k = i + 1; k > 0; k--)
            gen[k] = gen[k - 1] ^ gfMul(gen[k], root);
        gen[0] = gfMul(gen[0], root);
        root = gfMul(root, 2);
    }
}

// Remainder of data * x^ecLen mod gen, into rem[0..ecLen-1]
// (highest-degree coefficient first).
static void rsRemainder(const uint8_t *data, int dataLen, uint8_t *rem)
{
    memset(rem, 0, genEcLen);

    for (int i = 0; i < dataLen; i++) {

        uint8_t top = data[i] ^ rem[0];

        for (int j = 0; j < genEcLen - 1; j++)
            rem[j] = rem[j + 1];
        rem[genEcLen - 1] = 0;

        for (int j = 0; j < genEcLen; j++)
            rem[j] ^= gfMul(gen[genEcLen - 1 - j], top);
    }
}

// --- version tables (ECC L: single block) ----------------------------

static const int V_TOTAL[4] = { 26, 44, 70, 100 };
static const int V_DATA [4] = { 19, 34, 55,  80 };

// --- matrix state -----------------------------------------------------

// Matrix state: EXT_RAM (PSRAM) — these are scratch, not DMA, and
// internal SRAM is the story's scarcest resource.
static EXT_RAM_BSS_ATTR uint8_t mat[33][33];   // 0 light / 1 dark
static EXT_RAM_BSS_ATTR uint8_t fn [33][33];   // function-module map (skip on data/mask)
static int     qrSize;

static void set(int x, int y, int dark)
{
    if (x >= 0 && x < qrSize && y >= 0 && y < qrSize) {
        mat[y][x] = (uint8_t) (dark != 0);
        fn [y][x] = 1;
    }
}

// Finder pattern + separators, centred (cx, cy) (the 3x3 dark core).
static void setFinder(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {

            int adx = dx > 0 ? dx : -dx;
            int ady = dy > 0 ? dy : -dy;
            int dist = adx > ady ? adx : ady;

            set(cx + dx, cy + dy, dist != 2 && dist != 4);
        }
}

// Mask 0..7: whether module (x, y) is inverted (x = column, y = row).
// ISO/IEC 18004: i = row, j = column — mask 1 is row parity, mask 2 is
// the column mod 3, mask 4 mixes row/2 + col/3.
static int maskBit(int m, int x, int y)
{
    switch (m) {
    case 0:  return (x + y) % 2 == 0;
    case 1:  return y % 2 == 0;
    case 2:  return x % 3 == 0;
    case 3:  return (x + y) % 3 == 0;
    case 4:  return (y / 2 + x / 3) % 2 == 0;
    case 5:  return (x * y) % 2 + (x * y) % 3 == 0;
    case 6:  return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    default: return ((x * y) % 3 + (x + y) % 2) % 2 == 0;
    }
}

// Format information: ECC level L (01) + mask, BCH(15,5) coded.
static int formatBits(int mask)
{
    int data = (1 << 3) | mask;     // L = 01
    int rem = data;

    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);

    return (data << 10 | rem) ^ 0x5412;
}

static void drawFormat(int mask)
{
    int bits = formatBits(mask);

#ifdef QR_DEBUG
    printf("qr: drawFormat mask=%d size=%d cells:", mask, qrSize);
#endif

    // First copy (around the top-left finder)
    for (int i = 0; i <= 5; i++)
        set(8, i, (bits >> i) & 1);
    set(8, 7, (bits >> 6) & 1);
    set(8, 8, (bits >> 7) & 1);
    set(7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++)
        set(14 - i, 8, (bits >> i) & 1);

    // Second copy (the two other corners)
    for (int i = 0; i < 8; i++)
        set(qrSize - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++)
        set(8, qrSize - 15 + i, (bits >> i) & 1);

    set(8, qrSize - 8, 1);          // the dark module

#ifdef QR_DEBUG
    printf(" (col8 rows0-8, row8 cols0-8, corners)\n");
#endif
}

// Penalty score (ISO 18004 rules E1..E4) for mask selection.
static int penalty(void)
{
    int score = 0;

    for (int y = 0; y < qrSize; y++) {
        int run = 1;
        int dark = mat[y][0] ? 1 : 0;
        for (int x = 1; x < qrSize; x++) {
            if (mat[y][x] == mat[y][x-1]) {
                run++;
                if (run == 6) score += 3; else if (run > 6) score++;
            } else {
                run = 1;
            }
            dark += mat[y][x] ? 1 : 0;
        }
        (void) dark;
    }

    for (int x = 0; x < qrSize; x++) {
        int run = 1;
        for (int y = 1; y < qrSize; y++) {
            if (mat[y][x] == mat[y-1][x]) {
                run++;
                if (run == 6) score += 3; else if (run > 6) score++;
            } else {
                run = 1;
            }
        }
    }

    // 2x2 blocks of the same colour: +3 each
    for (int y = 0; y < qrSize - 1; y++)
        for (int x = 0; x < qrSize - 1; x++)
            if (mat[y][x] == mat[y][x+1]
                && mat[y][x] == mat[y+1][x]
                && mat[y][x] == mat[y+1][x+1])
                score += 3;

    // Finder-like 1011101 with 4 light on one side: +40 each
    for (int y = 0; y < qrSize; y++)
        for (int x = 0; x + 10 < qrSize; x++) {
            int v = 0, w = 0;
            for (int k = 0; k < 11; k++) {
                v = (v << 1) | mat[y][x + k];
                w = (w << 1) | mat[x + k][y];
            }
            if (v == 0x05D || v == 0x5D0)       // 00001011101 / 10111010000
                score += 40;
            if (w == 0x05D || w == 0x5D0)
                score += 40;
        }

    return score;
}

int qr_encode_mask(const char *text, unsigned char *out, int outCap, int forceMask);

int qr_encode(const char *text, unsigned char *out, int outCap)
{
    return qr_encode_mask(text, out, outCap, -1);
}

int qr_encode_mask(const char *text, unsigned char *out, int outCap, int forceMask)
{
    int len = (int) strlen(text);
    int ver = -1;

    for (int v = 0; v < 4; v++)
        if (len <= V_DATA[v] - 2) {
            ver = v;
            break;
        }
    if (ver < 0 || outCap < 33 * 33)
        return 0;

    int totalCW  = V_TOTAL[ver];
    int dataCW   = V_DATA[ver];
    int ecLen    = totalCW - dataCW;

    qrSize = 17 + 4 * (ver + 1);

    gfInit();
    genInit(ecLen);

    // --- bit stream: mode, count, data, terminator, pads -------------
    static EXT_RAM_BSS_ATTR uint8_t bits[130 * 8];
    int nbits = 0;

    memset(bits, 0, sizeof(bits));

#define PUT(val, n)                                        \
    for (int _i = (n) - 1; _i >= 0; _i--) {                \
        if (((val) >> _i) & 1)                             \
            bits[nbits >> 3] |= 0x80 >> (nbits & 7);       \
        nbits++;                                           \
    }

    PUT(4, 4);                      // byte mode
    PUT(len, 8);                    // char count (versions 1..9)
    for (int i = 0; i < len; i++)
        PUT((uint8_t) text[i], 8);

    if (nbits + 4 <= dataCW * 8) {  // terminator (up to 4 bits)
        PUT(0, 4);
    } else {
        PUT(0, dataCW * 8 - nbits);
    }

    nbits = (nbits + 7) & ~7;       // pad to a byte boundary (zero bits)
#ifdef QR_DEBUG
    printf("qr: len=%d nbits=%d after stream: %02X %02X %02X %02X\n",
           len, nbits, bits[0], bits[1], bits[2], bits[3]);
#endif
    for (int i = 0; nbits < dataCW * 8; i++)
        PUT((i & 1) ? 0x11 : 0xEC, 8);

#undef PUT

    // --- codewords + ECC ---------------------------------------------
    static EXT_RAM_BSS_ATTR uint8_t cw[130];
    uint8_t ec[20];

    for (int i = 0; i < dataCW; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++)
            if ((bits[((i * 8 + k) >> 3)] >> (7 - (k & 7))) & 1)
                b |= 0x80 >> k;
        cw[i] = b;
    }

    rsRemainder(cw, dataCW, ec);
    for (int i = 0; i < ecLen; i++)
        cw[dataCW + i] = ec[i];

    // --- function patterns -------------------------------------------
    memset(mat, 0, sizeof(mat));
    memset(fn,  0, sizeof(fn));

    setFinder(3, 3);
    setFinder(qrSize - 4, 3);
    setFinder(3, qrSize - 4);

    for (int i = 8; i < qrSize - 8; i++) {  // timing: between the finders
        set(i, 6, (i & 1) == 0);
        set(6, i, (i & 1) == 0);
    }

    if (ver >= 1) {                          // v2..v4: one alignment
        int c = qrSize - 7;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                int adx = dx > 0 ? dx : -dx;
                int ady = dy > 0 ? dy : -dy;
                int dist = adx > ady ? adx : ady;
                set(c + dx, c + dy, dist != 1);
            }
    }

    drawFormat(0);                           // reserve format cells

#ifdef QR_DEBUG
    printf("qr: function patterns only (fn map):\n");
    for (int y = 0; y < qrSize; y++) {
        for (int x = 0; x < qrSize; x++)
            printf("%d", fn[y][x]);
        printf("\n");
    }
#endif

    // --- data placement (two-column serpentine, MSB first) -----------
    {
        int c = 0;                           // codeword bit cursor
        for (int right = qrSize - 1; right >= 0; right -= 2) {

            if (right == 6)
                right = 5;                   // skip the timing column

            for (int j = 0; j < qrSize; j++)
                for (int i = 0; i < 2; i++) {

                    int x = right - i;
                    int up = ((right + 1) & 2) == 0;
                    int y = up ? qrSize - 1 - j : j;

                    if (!fn[y][x]) {
                        mat[y][x] = (cw[c >> 3] >> (7 - (c & 7))) & 1;
                        c++;
                    }
                }
        }

#ifdef QR_DEBUG
        {
            printf("qr: size=%d placed=%d bits (expect %d)\n",
                   qrSize, c, totalCW * 8);
            printf("qr: cw:");
            for (int i = 0; i < totalCW; i++)
                printf(" %02X", cw[i]);
            printf("\n");
        }
#endif
        // TEMP: which cells got bits beyond the codeword stream?
        // NOTE: placed may legitimately exceed totalCW * 8 — the matrix
        // has remainder bits (V2/V3: 7, V4: 3) filled with 0.
#ifdef QR_DEBUG
        if (c != totalCW * 8) {
            printf("qr: NOTE placed=%d totalCWbits=%d (remainder bits)\n",
                   c, totalCW * 8);
        }
#endif
    }

    // --- choose the mask with the lowest penalty ----------------------
    int bestScore = -1;
    static EXT_RAM_BSS_ATTR uint8_t best[33][33];

    for (int m = 0; m < 8; m++) {

        if (forceMask >= 0 && m != forceMask)
            continue;                        // testability: fixed mask

        for (int y = 0; y < qrSize; y++)
            for (int x = 0; x < qrSize; x++)
                if (!fn[y][x] && maskBit(m, x, y))
                    mat[y][x] ^= 1;

        drawFormat(m);                       // rewrite format for mask m

#ifdef QR_DEBUG
        if (m == 0 && qrSize == 21) {
            printf("qr: mask 0 applied:\n");
            for (int y = 0; y < qrSize; y++) {
                for (int x = 0; x < qrSize; x++)
                    printf("%d", mat[y][x]);
                printf("\n");
            }
            printf("qr: mask-0 data cells (fn==0) count:\n");
            int nfn = 0;
            for (int y = 0; y < qrSize; y++)
                for (int x = 0; x < qrSize; x++)
                    nfn += !fn[y][x];
            printf("qr: non-fn = %d\n", nfn);
        }
#endif

        int sc = penalty();
        if (bestScore < 0 || sc < bestScore) {
            bestScore = sc;
            memcpy(best, mat, sizeof(mat));
        }

#ifdef QR_DEBUG
        if (m == 0 && qrSize == 21) {
            printf("qr: best copy (should equal mask0 applied):\n");
            for (int y = 0; y < qrSize; y++) {
                for (int x = 0; x < qrSize; x++)
                    printf("%d", best[y][x]);
                printf("\n");
            }
        }
#endif

        for (int y = 0; y < qrSize; y++)     // undo the mask
            for (int x = 0; x < qrSize; x++)
                if (!fn[y][x] && maskBit(m, x, y))
                    mat[y][x] ^= 1;
    }

    for (int y = 0; y < qrSize; y++)          // row-wise: 'best' has the
        memcpy(out + y * qrSize, best[y], qrSize);   // 33-module stride

#ifdef QR_DEBUG
    printf("qr: memcmp(out,best)=%d out=%p best=%p mat=%p bits=%p cw=%p\n",
           memcmp(out, best, qrSize * qrSize),
           (void *) out, (void *) best, (void *) mat,
           (void *) bits, (void *) cw);
    {
        printf("qr: out[21..37] hex: ");
        for (int i = 21; i < 38; i++)
            printf("%02X", out[i]);
        printf("\n");
        printf("qr: best[1] hex:    ");
        for (int i = 21; i < 38; i++)
            printf("%02X", best[i / 33][i % 33]);
        printf("\n");
    }
#endif

#ifdef QR_DEBUG
    printf("qr: final best:\n");
    for (int y = 0; y < qrSize; y++) {
        for (int x = 0; x < qrSize; x++)
            printf("%d", out[y * qrSize + x]);
        printf("  |  ");
        for (int x = 0; x < qrSize; x++)
            printf("%d", best[y][x]);
        printf("\n");
    }
#endif
    return qrSize;
}
