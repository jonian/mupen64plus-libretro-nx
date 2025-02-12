/*
 * Texture Filtering
 * Version:  1.0
 *
 * Copyright (C) 2007  Hiroshi Morii   All Rights Reserved.
 * Email koolsmoky(at)users.sourceforge.net
 * Web   http://www.3dfxzone.it/koolsmoky
 *
 * this is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * this is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Make; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <thread>
#include "TxUtil.h"
#include "TxDbg.h"
#include <zlib.h>
#include <assert.h>

#if defined (OS_WINDOWS)
#include <malloc.h>
#elif defined (OS_MAC_OS_X)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#elif defined (OS_LINUX)
#include <unistd.h>
#include <malloc.h>
#endif

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

/*
 * Utilities
 ******************************************************************************/
static uint32 Uint64ToUint32(uint64_t t)
{
	return static_cast<uint32>((t & 0xFFFFFFFF) ^ (t >> 32));
}

uint32
TxUtil::checksumTx(uint8 *src, int width, int height, ColorFormat format)
{
	int dataSize = sizeofTx(width, height, format);
	return Uint64ToUint32(XXH3_64bits(src, dataSize));
}

int
TxUtil::sizeofTx(int width, int height, ColorFormat format)
{
	int dataSize = 0;

	/* a lookup table for the shifts would be better */
	if (format == graphics::internalcolorFormat::COLOR_INDEX8) {
		dataSize = width * height;
	} else if (format == graphics::internalcolorFormat::RGBA4 ||
			   format == graphics::internalcolorFormat::RGB5_A1 ||
			   format == graphics::internalcolorFormat::RGB8) {
		dataSize = (width * height) << 1;
	} else if (format == graphics::internalcolorFormat::RGBA8) {
		dataSize = (width * height) << 2;
	} else {
		/* unsupported format */
		DBG_INFO(80, wst("Error: cannot get size. unsupported gfmt:%x\n"), format);
	}

	return dataSize;
}

uint64
TxUtil::checksum64(uint8 *src, int width, int height, int size, int rowStride, uint8 *palette)
{
	/* Rice CRC32 for now. We can switch this to Jabo MD5 or
   * any other custom checksum.
   * TODO: use *_HIRESTEXTURE option. */
	/* Returned value is 64bits: hi=palette crc32 low=texture crc32 */

	if (!src) return 0;

	uint64 crc64Ret = 0;

	if (palette) {
		uint32 crc32 = 0, cimax = 0;
		switch (size & 0xff) {
		case 1:
			if (RiceCRC32_CI8(src, width, height, rowStride, &crc32, &cimax)) {
				crc64Ret = (uint64)RiceCRC32(palette, cimax + 1, 1, 2, 512);
				crc64Ret <<= 32;
				crc64Ret |= (uint64)crc32;
			}
		break;
		case 0:
			if (RiceCRC32_CI4(src, width, height, rowStride, &crc32, &cimax)) {
				crc64Ret = (uint64)RiceCRC32(palette, cimax + 1, 1, 2, 32);
				crc64Ret <<= 32;
				crc64Ret |= (uint64)crc32;
			}
		}
	}
	if (!crc64Ret) {
		crc64Ret = (uint64)RiceCRC32(src, width, height, size, rowStride);
	}

	return crc64Ret;
}

uint64
TxUtil::checksum64strong(uint8 *src, int width, int height, int size, int rowStride, uint8 *palette)
{
	/* XXH3_64bits for strong 32bit texture hash. */
    /* Returned value is 64bits: hi=palette crc32 low=texture crc32 */

	if (!src)
		return 0;

	uint64 crc64Ret = 0;

	if (palette) {
		uint32 crc32 = 0, cimax = 0;
		switch (size & 0xff) {
		case 1:
			if (StrongCRC32_CI8(src, width, height, rowStride, &crc32, &cimax)) {
				crc64Ret = StrongCRC32(palette, cimax + 1, 1, 2, 512);
				crc64Ret <<= 32;
				crc64Ret |= crc32;
			}
			break;
		case 0:
			if (StrongCRC32_CI4(src, width, height, rowStride, &crc32, &cimax)) {
				crc64Ret = StrongCRC32(palette, cimax + 1, 1, 2, 32);
				crc64Ret <<= 32;
				crc64Ret |= crc32;
			}
		}
	}

	if (!crc64Ret) {
		crc64Ret = StrongCRC32(src, width, height, size, rowStride);
	}

	return crc64Ret;
}

/* Rice CRC32 for hires texture packs */
/* NOTE: The following is used in Glide64 to calculate the CRC32
 * for Rice hires texture packs.
 *
 * BYTE* addr = (BYTE*)(gfx.RDRAM +
 *                     rdp.addr[rdp.tiles[tile].t_mem] +
 *                     (rdp.tiles[tile].ul_t * bpl) +
 *                     (((rdp.tiles[tile].ul_s<<rdp.tiles[tile].size)+1)>>1));
 * RiceCRC32(addr,
 *          rdp.tiles[tile].width,
 *          rdp.tiles[tile].height,
 *          (unsigned short)(rdp.tiles[tile].format << 8 | rdp.tiles[tile].size),
 *          bpl);
 */
uint32
TxUtil::RiceCRC32(const uint8* src, int width, int height, int size, int rowStride)
{
	/* NOTE: bytes_per_width must be equal or larger than 4 */

	uint32 crc32Ret = 0;
	const uint32 bytesPerLine = width << size >> 1;

	try {
#ifdef WIN32_ASM
		__asm {
			push ebx;
			push esi;
			push edi;

			mov ecx, dword ptr [src];
			mov eax, dword ptr [height];
			mov edx, 0;
			dec eax;

loop2:
			mov ebx, dword ptr[bytesPerLine];
			sub ebx, 4;

loop1:
			mov esi, dword ptr [ecx+ebx];
			xor esi, ebx;
			rol edx, 4;
			add edx, esi;
			sub ebx, 4;
			jge loop1;

			xor esi, eax;
			add edx, esi;
			add ecx, dword ptr [rowStride];
			dec eax;
			jge loop2;

			mov dword ptr [crc32Ret], edx;

			pop edi;
			pop esi;
			pop ebx;
		}
#else
		int y = height - 1;
		do {
			uint32 esi = 0;
			int x = bytesPerLine - 4;
			do {
				esi = *(uint32*)(src + x);
				esi ^= x;

				crc32Ret = (crc32Ret << 4) + ((crc32Ret >> 28) & 15);
				crc32Ret += esi;
				x -= 4;
			} while (x >= 0);
			esi ^= y;
			crc32Ret += esi;
			src += rowStride;
			--y;
		} while (y >= 0);
#endif
	} catch(...) {
		DBG_INFO(80, wst("Error: RiceCRC32 exception!\n"));
	}

	return crc32Ret;
}

static
uint8 CalculateMaxCI8b(const uint8* src, uint32 width, uint32 height, uint32 rowStride)
{
	uint8 val = 0;
	for (uint32 y = 0; y < height; ++y) {
		const uint8 * buf = src + rowStride * y;
		for (uint32 x = 0; x<width; ++x) {
			if (buf[x] > val)
				val = buf[x];
			if (val == 0xFF)
				return 0xFF;
		}
	}
	return val;
}

static
uint8 CalculateMaxCI4b(const uint8* src, uint32 width, uint32 height, uint32 rowStride)
{
	uint8 val = 0;
	uint8 val1, val2;
	width >>= 1;
	for (uint32 y = 0; y < height; ++y) {
		const uint8 * buf = src + rowStride * y;
		for (uint32 x = 0; x<width; ++x) {
			val1 = buf[x] >> 4;
			val2 = buf[x] & 0xF;
			if (val1 > val) val = val1;
			if (val2 > val) val = val2;
			if (val == 0xF)
				return 0xF;
		}
	}
	return val;
}

boolean
TxUtil::RiceCRC32_CI4(const uint8* src, int width, int height, int rowStride,
					  uint32* crc32, uint32* cimax)
{
	/* NOTE: bytes_per_width must be equal or larger than 4 */

	uint32 crc32Ret = 0;
	uint32 cimaxRet = 0;
	const uint32 bytes_per_width = width >> 1;

	/*if (bytes_per_width < 4) return 0;*/

	/* 4bit CI */
	try {
#ifdef WIN32_ASM
		__asm {
			push ebx;
			push esi;
			push edi;

			mov ecx, dword ptr [src];
			mov eax, dword ptr [height];
			mov edx, 0;
			mov edi, 0;
			dec eax;

loop2:
			mov ebx, dword ptr [bytes_per_width];
			sub ebx, 4;

loop1:
			mov esi, dword ptr [ecx+ebx];

			cmp edi, 0x0000000f;
			je findmax0;

			push ecx;
			mov ecx, esi;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax8;
			mov edi, ecx;

findmax8:
			mov ecx, esi;
			shr ecx, 4;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax7;
			mov edi, ecx;

findmax7:
			mov ecx, esi;
			shr ecx, 8;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax6;
			mov edi, ecx;

findmax6:
			mov ecx, esi;
			shr ecx, 12;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax5;
			mov edi, ecx;

findmax5:
			mov ecx, esi;
			shr ecx, 16;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax4;
			mov edi, ecx;

findmax4:
			mov ecx, esi;
			shr ecx, 20;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax3;
			mov edi, ecx;

findmax3:
			mov ecx, esi;
			shr ecx, 24;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax2;
			mov edi, ecx;

findmax2:
			mov ecx, esi;
			shr ecx, 28;
			and ecx, 0x0000000f;
			cmp ecx, edi;
			jb  findmax1;
			mov edi, ecx;

findmax1:
			pop ecx;

findmax0:
			xor esi, ebx;
			rol edx, 4;
			add edx, esi;
			sub ebx, 4;
			jge loop1;

			xor esi, eax;
			add edx, esi;
			add ecx, dword ptr [rowStride];
			dec eax;
			jge loop2;

			mov dword ptr [crc32Ret], edx;
			mov dword ptr [cimaxRet], edi;

			pop edi;
			pop esi;
			pop ebx;
		}
#else
		crc32Ret = RiceCRC32(src, width, height, 0, rowStride);
		cimaxRet = CalculateMaxCI4b(src, width, height, rowStride);
#endif
	} catch(...) {
		DBG_INFO(80, wst("Error: RiceCRC32 exception!\n"));
	}

	*crc32 = crc32Ret;
	*cimax = cimaxRet;

	return 1;
}

boolean
TxUtil::RiceCRC32_CI8(const uint8* src, int width, int height, int rowStride,
					  uint32* crc32, uint32* cimax)
{
	/* NOTE: bytes_per_width must be equal or larger than 4 */

	uint32 crc32Ret = 0;
	uint32 cimaxRet = 0;

	/* 8bit CI */
	try {
#ifdef WIN32_ASM
		const uint32 bytes_per_width = width;
		__asm {
			push ebx;
			push esi;
			push edi;

			mov ecx, dword ptr [src];
			mov eax, dword ptr [height];
			mov edx, 0;
			mov edi, 0;
			dec eax;

loop2:
			mov ebx, dword ptr [bytes_per_width];
			sub ebx, 4;

loop1:
			mov esi, dword ptr [ecx+ebx];

			cmp edi, 0x000000ff;
			je findmax0;

			push ecx;
			mov ecx, esi;
			and ecx, 0x000000ff;
			cmp ecx, edi;
			jb  findmax4;
			mov edi, ecx;

findmax4:
			mov ecx, esi;
			shr ecx, 8;
			and ecx, 0x000000ff;
			cmp ecx, edi;
			jb  findmax3;
			mov edi, ecx;

findmax3:
			mov ecx, esi;
			shr ecx, 16;
			and ecx, 0x000000ff;
			cmp ecx, edi;
			jb  findmax2;
			mov edi, ecx;

findmax2:
			mov ecx, esi;
			shr ecx, 24;
			and ecx, 0x000000ff;
			cmp ecx, edi;
			jb  findmax1;
			mov edi, ecx;

findmax1:
			pop ecx;

findmax0:
			xor esi, ebx;
			rol edx, 4;
			add edx, esi;
			sub ebx, 4;
			jge loop1;

			xor esi, eax;
			add edx, esi;
			add ecx, dword ptr [rowStride];
			dec eax;
			jge loop2;

			mov dword ptr [crc32Ret], edx;
			mov dword ptr [cimaxRet], edi;

			pop edi;
			pop esi;
			pop ebx;
		}
#else
		crc32Ret = RiceCRC32(src, width, height, 1, rowStride);
		cimaxRet = CalculateMaxCI8b(src, width, height, rowStride);
#endif
	} catch(...) {
		DBG_INFO(80, wst("Error: RiceCRC32 exception!\n"));
	}

	*crc32 = crc32Ret;
	*cimax = cimaxRet;

	return 1;
}

uint32
TxUtil::StrongCRC32(const uint8* src, int width, int height, int size, int rowStride)
{
	/* NOTE: bytesPerLine must be equal or larger than 4 */
	const uint32 bytesPerLine = width << size >> 1;

	u64 crc = UINT64_MAX;
	std::vector<uint8> buf(static_cast<uint32>(height) * std::max(bytesPerLine, static_cast<uint32>(rowStride)));
	uint8* pData = buf.data();
	try {
		for (int y = 0; y < height; ++y) {
			if (bytesPerLine < 4) {
				// bytesPerLine must be >= 4, but if it less than 4, reproduce RiceCRC behavior,
				// that is read bytes before provided source address.
				memcpy(pData, src - 4 + bytesPerLine, 4);
				pData += 4;
			}
			else {
				memcpy(pData, src, bytesPerLine);
				pData += bytesPerLine;
			}
			src += rowStride;
		}
		crc = XXH3_64bits(buf.data(), static_cast<size_t>(pData - buf.data()));
	}
	catch (...) {
		DBG_INFO(80, wst("Error: StrongCRC32 exception!\n"));
	}

	return Uint64ToUint32(crc);
}

boolean
TxUtil::StrongCRC32_CI4(const uint8* src, int width, int height, int rowStride,
					  uint32* crc32, uint32* cimax)
{
	/* NOTE: bytes_per_width must be equal or larger than 4 */

	/* 4bit CI */
	try {
		uint32 crc32Ret = StrongCRC32(src, width, height, 0, rowStride);
		uint32 cimaxRet = CalculateMaxCI4b(src, width, height, rowStride);
		*crc32 = crc32Ret;
		*cimax = cimaxRet;
		return 1;
	} catch(...) {
		DBG_INFO(80, wst("Error: RiceCRC32 exception!\n"));
	}
	return 0;
}

boolean
TxUtil::StrongCRC32_CI8(const uint8* src, int width, int height, int rowStride,
	uint32* crc32, uint32* cimax)
{
	/* NOTE: bytes_per_width must be equal or larger than 4 */

	/* 8bit CI */
	try {
		uint32 crc32Ret = StrongCRC32(src, width, height, 1, rowStride);
		uint32 cimaxRet = CalculateMaxCI8b(src, width, height, rowStride);
		*crc32 = crc32Ret;
		*cimax = cimaxRet;
		return 1;
	}
	catch (...) {
		DBG_INFO(80, wst("Error: RiceCRC32 exception!\n"));
	}
	return 0;
}

uint32 TxUtil::getNumberofProcessors()
{
	uint32 numcore = 1; //std::thread::hardware_concurrency();
	if (numcore > MAX_NUMCORE) numcore = MAX_NUMCORE;
	DBG_INFO(80, wst("Number of processors : %d\n"), numcore);
	return numcore;
}

/*
 * Memory buffers for texture manipulations
 ******************************************************************************/
TxMemBuf::TxMemBuf()
{
	for (uint32 i = 0; i < 2; i++) {
		_tex[i] = nullptr;
		_size[i] = 0;
	}
}

TxMemBuf::~TxMemBuf()
{
	shutdown();
}

boolean
TxMemBuf::init(int maxwidth, int maxheight)
{
	try {
		for (uint32 i = 0; i < 2; i++) {
			if (_tex[i] == nullptr) {
				_tex[i] = (uint8 *)malloc(maxwidth * maxheight * 4);
				_size[i] = maxwidth * maxheight * 4;
			}

			if (_tex[i] == nullptr) {
				shutdown();
				return 0;
			}
		}

		if (_bufs.empty()) {
			const int numcore = TxUtil::getNumberofProcessors();
			const size_t numBuffers = numcore*2;
			_bufs.resize(numBuffers);
		}
	} catch(std::bad_alloc) {
		shutdown();
		return 0;
	}

	return 1;
}

void
TxMemBuf::shutdown()
{
	for (int i = 0; i < 2; i++) {
		if (_tex[i] != nullptr)
			free(_tex[i]);
		_tex[i] = nullptr;
		_size[i] = 0;
	}

	_bufs.clear();
}

uint8*
TxMemBuf::get(uint32 num)
{
	assert(num < 2);
	return _tex[num];
}

uint32
TxMemBuf::size_of(uint32 num)
{
	assert(num < 2);
	return _size[num];
}

uint32*
TxMemBuf::getThreadBuf(uint32 threadIdx, uint32 num, uint32 size)
{
	assert(num < 2);
	const auto idx = threadIdx * 2 + num;
	auto& buf = _bufs[idx];

	if (buf.size() < size) {
		try {
			buf.resize(size, 0);
		} catch(std::bad_alloc) {
			return nullptr;
		}
	}

	return buf.data();
}

void setTextureFormat(ColorFormat internalFormat, GHQTexInfo * info)
{
	info->format = u32(internalFormat);
	if (internalFormat == graphics::internalcolorFormat::RGBA8) {
		info->texture_format = static_cast<unsigned short>(u32(graphics::colorFormat::RGBA));
		info->pixel_type = static_cast<unsigned short>(u32(graphics::datatype::UNSIGNED_BYTE));
	} else if (internalFormat == graphics::internalcolorFormat::RGB8) {
		info->texture_format = static_cast<unsigned short>(u32(graphics::colorFormat::RED_GREEN_BLUE));
		info->pixel_type = static_cast<unsigned short>(u32(graphics::datatype::UNSIGNED_SHORT_5_6_5));
	} else if (internalFormat == graphics::internalcolorFormat::RGBA4) {
		info->texture_format = static_cast<unsigned short>(u32(graphics::colorFormat::RGBA));
		info->pixel_type = static_cast<unsigned short>(u32(graphics::datatype::UNSIGNED_SHORT_4_4_4_4));
	} else if (internalFormat == graphics::internalcolorFormat::RGB5_A1) {
		info->texture_format = static_cast<unsigned short>(u32(graphics::colorFormat::RGBA));
		info->pixel_type = static_cast<unsigned short>(u32(graphics::datatype::UNSIGNED_SHORT_5_5_5_1));
	} else {
		info->texture_format = static_cast<unsigned short>(u32(graphics::colorFormat::RGBA));
		info->pixel_type = static_cast<unsigned short>(u32(graphics::datatype::UNSIGNED_BYTE));
	}
}
