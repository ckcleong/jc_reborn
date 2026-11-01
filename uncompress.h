/*
 *  This file is part of 'Johnny Reborn'
 *
 *  An open-source engine for the classic
 *  'Johnny Castaway' screensaver by Sierra.
 *
 *  Copyright (C) 2019 Jeremie GUILLAUME
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

uint8 *uncompress(FILE *f, uint8 compressionMethod, uint32 inSize, uint32 outSize);

// Same decompressors, but the uncompressed bytes are written into a
// caller-provided buffer ('outData', at least 'outSize' bytes) instead of a
// fresh safe_malloc() block. Nothing may be allocated: this is the streaming
// form used to decode resources straight out of the resource file (SCR
// backgrounds: the uncompressed blob is the surface bitmap itself, so a
// in-place decode makes the 112-153 KB blob never exist in RAM).
void uncompressIntoBuf(FILE *f, uint8 compressionMethod, uint32 inSize,
                       uint32 outSize, uint8 *outData);

