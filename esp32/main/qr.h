/*
 * Minimal QR encoder for the ESP32 setup screen.
 *
 * Byte mode only, versions 1..4, ECC level L — every supported
 * (version, ECC L) combination is a single Reed-Solomon block, so no
 * codeword de-interleaving is needed. Capacity: 17/32/53/78 bytes.
 *
 * qr_encode() renders 'text' into 'out' as a size*size 0/1 module
 * matrix (row-major, 0 = light, 1 = dark, NO quiet zone — the caller
 * draws the border) and returns the matrix size (21..33), or 0 if the
 * text does not fit.
 *
 * qr_encode_mask() additionally forces a mask pattern (0..7) for
 * testing — mask selection is otherwise ISO-optimal.
 */
#pragma once

int qr_encode(const char *text, unsigned char *out, int outCap);
int qr_encode_mask(const char *text, unsigned char *out, int outCap,
                   int forceMask);
