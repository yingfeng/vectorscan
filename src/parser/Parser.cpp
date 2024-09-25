#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"

#line 1 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
/*
 * Copyright (c) 2015-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief Parser code (generated with Ragel from Parser.rl).
 */

#include "config.h"

/* Parser.cpp is a built source, may not be in same dir as parser files */
#include "parser/check_refs.h"
#include "parser/control_verbs.h"
#include "parser/ComponentAlternation.h"
#include "parser/ComponentAssertion.h"
#include "parser/ComponentAtomicGroup.h"
#include "parser/ComponentBackReference.h"
#include "parser/ComponentBoundary.h"
#include "parser/ComponentByte.h"
#include "parser/ComponentClass.h"
#include "parser/ComponentCondReference.h"
#include "parser/ComponentEmpty.h"
#include "parser/ComponentEUS.h"
#include "parser/Component.h"
#include "parser/ComponentRepeat.h"
#include "parser/ComponentSequence.h"
#include "parser/ComponentWordBoundary.h"
#include "parser/parse_error.h"
#include "parser/Parser.h"
#include "ue2common.h"
#include "util/compare.h"
#include "util/flat_containers.h"
#include "util/unicode_def.h"
#include "util/verify_types.h"

#include <cassert>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace ue2 {

#define PUSH_SEQUENCE do {\
        sequences.push_back(ExprState(currentSeq, (size_t)(ts - ptr), \
                mode)); \
    } while(0)
#define POP_SEQUENCE do {\
        currentSeq = sequences.back().seq; \
        mode = sequences.back().mode; \
        sequences.pop_back(); \
    } while(0)

namespace {

/** \brief Structure representing current state as we're parsing (current
 * sequence, current options). Stored in the 'sequences' vector. */
struct ExprState {
    ExprState(ComponentSequence *seq_in, size_t offset,
              const ParseMode &mode_in) :
        seq(seq_in), seqOffset(offset), mode(mode_in) {}

    ComponentSequence *seq; //!< current sequence
    size_t seqOffset; //!< offset seq was entered, for error reporting
    ParseMode mode; //!< current mode flags
};

} // namespace

static
unsigned parseAsDecimal(unsigned oct) {
    // The input was parsed as octal, but should have been parsed as decimal.
    // Deconstruct the octal number and reconstruct into decimal
    unsigned ret = 0;
    unsigned multiplier = 1;
    while (oct) {
        ret += (oct & 0x7) * multiplier;
        oct >>= 3;
        multiplier *= 10;
    }
    return ret;
}

/** \brief Maximum value for a positive integer. We use INT_MAX, as that's what
 * PCRE uses. */
static constexpr u32 MAX_NUMBER = INT_MAX;

static
void pushDec(u32 *acc, char raw_digit) {
    assert(raw_digit >= '0' && raw_digit <= '9');
    u32 digit_val = raw_digit - '0';

    // Ensure that we don't overflow.
    u64a val = ((u64a)*acc * 10) + digit_val;
    if (val > MAX_NUMBER) {
        throw LocatedParseError("Number is too big");
    }

    *acc = verify_u32(val);
}

static
void pushOct(u32 *acc, char raw_digit) {
    assert(raw_digit >= '0' && raw_digit <= '7');
    u32 digit_val = raw_digit - '0';

    // Ensure that we don't overflow.
    u64a val = ((u64a)*acc * 8) + digit_val;
    if (val > MAX_NUMBER) {
        throw LocatedParseError("Number is too big");
    }

    *acc = verify_u32(val);
}

static
void throwInvalidRepeat(void) {
    throw LocatedParseError("Invalid repeat");
}

static
void throwInvalidUtf8(void) {
    throw ParseError("Expression is not valid UTF-8.");
}

/**
 * Adds the given child component to the parent sequence, returning a pointer
 * to the new (child) "current sequence".
 */
static
ComponentSequence *enterSequence(ComponentSequence *parent,
                                 unique_ptr<ComponentSequence> child) {
    assert(parent);
    assert(child);

    ComponentSequence *seq = child.get();
    parent->addComponent(std::move(child));
    return seq; // cppcheck-suppress returnDanglingLifetime
}

static
void addLiteral(ComponentSequence *currentSeq, char c, const ParseMode &mode) {
    if (mode.utf8 && mode.caseless) {
        /* leverage ComponentClass to generate the vertices */
        auto cc = getComponentClass(mode);
        assert(cc);
        cc->add(c);
        cc->finalize();
        currentSeq->addComponent(std::move(cc));
    } else {
        currentSeq->addComponent(getLiteralComponentClass(c, mode.caseless));
    }
}

static
void addEscaped(ComponentSequence *currentSeq, unichar accum,
                const ParseMode &mode, const char *err_msg) {
    if (mode.utf8) {
        /* leverage ComponentClass to generate the vertices */
        auto cc = getComponentClass(mode);
        assert(cc);
        cc->add(accum);
        cc->finalize();
        currentSeq->addComponent(std::move(cc));
    } else {
        if (accum > 255) {
            throw LocatedParseError(err_msg);
        }
        addLiteral(currentSeq, (char)accum, mode);
    }
}

static
void addEscapedOctal(ComponentSequence *currentSeq, unichar accum,
                     const ParseMode &mode) {
    addEscaped(currentSeq, accum, mode, "Octal value is greater than \\377");
}

static
void addEscapedHex(ComponentSequence *currentSeq, unichar accum,
                   const ParseMode &mode) {
    addEscaped(currentSeq, accum, mode,
               "Hexadecimal value is greater than \\xFF");
}

#define SLASH_C_ERROR "\\c must be followed by an ASCII character"

static
u8 decodeCtrl(char raw) {
    if (raw & 0x80) {
        throw LocatedParseError(SLASH_C_ERROR);
    }
    return mytoupper(raw) ^ 0x40;
}

static
unichar readUtf8CodePoint2c(const char *s) {
    auto *ts = reinterpret_cast<const u8 *>(s);
    assert(ts[0] >= 0xc0 && ts[0] < 0xe0);
    assert(ts[1] >= 0x80 && ts[1] < 0xc0);
    unichar val = ts[0] & 0x1f;
    val <<= 6;
    val |= ts[1] & 0x3f;
    DEBUG_PRINTF("utf8 %02hhx %02hhx ->\\x{%x}\n", ts[0],
                 ts[1], val);
    return val;
}

static
unichar readUtf8CodePoint3c(const char *s) {
    auto *ts = reinterpret_cast<const u8 *>(s);
    assert(ts[0] >= 0xe0 && ts[0] < 0xf0);
    assert(ts[1] >= 0x80 && ts[1] < 0xc0);
    assert(ts[2] >= 0x80 && ts[2] < 0xc0);
    unichar val = ts[0] & 0x0f;
    val <<= 6;
    val |= ts[1] & 0x3f;
    val <<= 6;
    val |= ts[2] & 0x3f;
    DEBUG_PRINTF("utf8 %02hhx %02hhx %02hhx ->\\x{%x}\n", ts[0],
                 ts[1], ts[2], val);
    return val;
}

static
unichar readUtf8CodePoint4c(const char *s) {
    auto *ts = reinterpret_cast<const u8 *>(s);
    assert(ts[0] >= 0xf0 && ts[0] < 0xf8);
    assert(ts[1] >= 0x80 && ts[1] < 0xc0);
    assert(ts[2] >= 0x80 && ts[2] < 0xc0);
    assert(ts[3] >= 0x80 && ts[3] < 0xc0);
    unichar val = ts[0] & 0x07;
    val <<= 6;
    val |= ts[1] & 0x3f;
    val <<= 6;
    val |= ts[2] & 0x3f;
    val <<= 6;
    val |= ts[3] & 0x3f;
    DEBUG_PRINTF("utf8 %02hhx %02hhx %02hhx %02hhx ->\\x{%x}\n", ts[0],
                 ts[1], ts[2], ts[3], val);
    return val;
}


#line 1910 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"



#line 276 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
static const short _regex_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1, 
	3, 1, 4, 1, 7, 1, 8, 1, 
	9, 1, 10, 1, 11, 1, 12, 1, 
	13, 1, 15, 1, 16, 1, 17, 1, 
	18, 1, 19, 1, 20, 1, 21, 1, 
	22, 1, 23, 1, 24, 1, 25, 1, 
	26, 1, 27, 1, 28, 1, 29, 1, 
	30, 1, 31, 1, 32, 1, 33, 1, 
	34, 1, 35, 1, 36, 1, 37, 1, 
	38, 1, 39, 1, 40, 1, 41, 1, 
	42, 1, 43, 1, 44, 1, 45, 1, 
	46, 1, 47, 1, 48, 1, 49, 1, 
	50, 1, 51, 1, 52, 1, 53, 1, 
	54, 1, 55, 1, 56, 1, 57, 1, 
	58, 1, 59, 1, 60, 1, 61, 1, 
	62, 1, 63, 1, 64, 1, 65, 1, 
	66, 1, 67, 1, 68, 1, 69, 1, 
	70, 1, 71, 1, 72, 1, 73, 1, 
	74, 1, 75, 1, 76, 1, 77, 1, 
	78, 1, 79, 1, 80, 1, 81, 1, 
	82, 1, 83, 1, 84, 1, 85, 1, 
	86, 1, 87, 1, 88, 1, 89, 1, 
	90, 1, 91, 1, 92, 1, 93, 1, 
	94, 1, 95, 1, 96, 1, 97, 1, 
	98, 1, 99, 1, 100, 1, 101, 1, 
	102, 1, 103, 1, 104, 1, 105, 1, 
	106, 1, 107, 1, 108, 1, 109, 1, 
	110, 1, 111, 1, 112, 1, 113, 1, 
	114, 1, 115, 1, 116, 1, 117, 1, 
	118, 1, 119, 1, 120, 1, 121, 1, 
	122, 1, 123, 1, 124, 1, 125, 1, 
	126, 1, 127, 1, 128, 1, 129, 1, 
	130, 1, 131, 1, 132, 1, 133, 1, 
	134, 1, 135, 1, 136, 1, 137, 1, 
	138, 1, 139, 1, 140, 1, 141, 1, 
	142, 1, 143, 1, 144, 1, 145, 1, 
	146, 1, 147, 1, 148, 1, 149, 1, 
	150, 1, 151, 1, 152, 1, 153, 1, 
	154, 1, 155, 1, 156, 1, 157, 1, 
	158, 1, 159, 1, 160, 1, 161, 1, 
	162, 1, 163, 1, 164, 1, 165, 1, 
	166, 1, 167, 1, 168, 1, 169, 1, 
	170, 1, 171, 1, 172, 1, 173, 1, 
	174, 1, 175, 1, 176, 1, 177, 1, 
	178, 1, 179, 1, 180, 1, 181, 1, 
	182, 1, 183, 1, 184, 1, 185, 1, 
	186, 1, 187, 1, 188, 1, 189, 1, 
	190, 1, 191, 1, 192, 1, 193, 1, 
	194, 1, 195, 1, 196, 1, 197, 1, 
	198, 1, 199, 1, 200, 1, 201, 1, 
	202, 1, 203, 1, 204, 1, 205, 1, 
	206, 1, 207, 1, 208, 1, 209, 1, 
	210, 1, 211, 1, 212, 1, 213, 1, 
	214, 1, 215, 1, 216, 1, 217, 1, 
	218, 1, 219, 1, 220, 1, 221, 1, 
	222, 1, 223, 1, 224, 1, 225, 1, 
	226, 1, 227, 1, 228, 1, 229, 1, 
	230, 1, 231, 1, 232, 1, 233, 1, 
	234, 1, 235, 1, 236, 1, 237, 1, 
	240, 1, 242, 1, 243, 1, 244, 1, 
	245, 1, 246, 1, 247, 1, 248, 1, 
	249, 1, 250, 1, 251, 1, 252, 1, 
	253, 1, 254, 1, 255, 1, 256, 1, 
	257, 1, 258, 1, 259, 1, 260, 1, 
	261, 1, 262, 1, 263, 1, 264, 1, 
	265, 1, 266, 1, 267, 1, 268, 1, 
	269, 1, 270, 1, 271, 1, 272, 1, 
	273, 1, 274, 1, 275, 1, 276, 1, 
	277, 1, 278, 1, 279, 1, 280, 1, 
	281, 1, 282, 1, 283, 1, 284, 1, 
	285, 1, 286, 1, 287, 1, 288, 1, 
	289, 1, 290, 1, 291, 1, 292, 1, 
	293, 1, 294, 1, 295, 1, 296, 1, 
	297, 1, 298, 1, 299, 1, 300, 1, 
	301, 1, 302, 1, 303, 1, 307, 1, 
	308, 1, 309, 1, 310, 1, 311, 1, 
	312, 1, 313, 1, 314, 1, 315, 1, 
	316, 1, 317, 1, 318, 1, 319, 1, 
	320, 1, 321, 1, 322, 1, 323, 1, 
	324, 1, 325, 1, 326, 1, 327, 1, 
	328, 1, 329, 1, 330, 1, 331, 1, 
	332, 1, 333, 1, 334, 1, 335, 1, 
	336, 1, 337, 1, 338, 1, 342, 1, 
	343, 1, 344, 1, 345, 1, 346, 1, 
	347, 1, 348, 1, 349, 1, 350, 1, 
	352, 1, 353, 1, 354, 1, 355, 1, 
	356, 1, 357, 1, 358, 1, 359, 1, 
	360, 1, 361, 1, 362, 1, 363, 1, 
	364, 1, 365, 1, 366, 1, 367, 1, 
	368, 1, 369, 1, 370, 1, 371, 1, 
	372, 1, 373, 1, 374, 1, 375, 1, 
	376, 1, 377, 1, 378, 1, 379, 1, 
	380, 1, 381, 1, 382, 1, 383, 1, 
	384, 1, 385, 1, 386, 1, 387, 1, 
	388, 1, 389, 1, 390, 1, 391, 1, 
	392, 1, 393, 1, 394, 1, 395, 1, 
	396, 1, 397, 1, 398, 1, 399, 1, 
	400, 1, 401, 1, 402, 1, 403, 1, 
	404, 1, 405, 1, 406, 1, 407, 1, 
	408, 1, 409, 1, 410, 1, 411, 1, 
	412, 1, 413, 1, 414, 1, 415, 1, 
	416, 1, 417, 1, 418, 1, 419, 1, 
	420, 1, 421, 1, 422, 1, 423, 1, 
	424, 1, 425, 1, 426, 1, 427, 1, 
	428, 1, 429, 1, 430, 1, 431, 1, 
	432, 1, 433, 1, 434, 1, 435, 1, 
	436, 2, 3, 0, 2, 4, 5, 2, 
	5, 1, 2, 9, 10, 2, 9, 238, 
	2, 9, 239, 2, 9, 339, 2, 10, 
	1, 2, 10, 340, 2, 10, 341, 2, 
	11, 241, 2, 11, 351, 2, 12, 241, 
	2, 12, 351, 2, 13, 241, 2, 13, 
	351, 2, 14, 375, 2, 14, 376, 2, 
	25, 0, 2, 25, 3, 2, 25, 6, 
	2, 25, 14, 3, 25, 5, 306, 3, 
	25, 10, 305, 3, 25, 14, 15, 4, 
	25, 9, 304, 10
};

static const short _regex_to_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 41, 
	0, 41, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 41, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 41, 0, 0, 41, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 41, 41, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 41, 0, 41, 0, 
	0, 0, 0, 41, 0, 0, 0, 0, 
	41, 41
};

static const short _regex_from_state_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 43, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 43, 0, 0, 43, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 43, 43, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 43, 0, 43, 0, 
	0, 0, 0, 43, 0, 0, 0, 0, 
	43, 43
};

static const short _regex_eof_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 39, 
	39, 39, 39, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 
	0, 0
};

static const int regex_start = 746;
static const int regex_error = 0;

static const int regex_en_readVerb = 787;
static const int regex_en_readUCP = 790;
static const int regex_en_readBracedUCP = 559;
static const int regex_en_readUCPSingle = 818;
static const int regex_en_charClassGuts = 819;
static const int regex_en_readClass = 836;
static const int regex_en_readQuotedLiteral = 838;
static const int regex_en_readQuotedClass = 843;
static const int regex_en_readComment = 848;
static const int regex_en_readNewlineTerminatedComment = 849;
static const int regex_en_main = 746;


#line 1913 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"

/** \brief Main parser call, returns root Component or nullptr. */
unique_ptr<Component> parse(const char *ptr, ParseMode &globalMode) {
    assert(ptr);

    const char *p = ptr;
    const char *pe = ptr + strlen(ptr);

    // First, read the control verbs, set any global mode flags and move the
    // ptr forward.
    p = read_control_verbs(p, pe, 0, globalMode);

    const char *eof = pe;
    int cs;
    UNUSED int act;
    int top;
    vector<int> stack;
    const char *ts, *te;
    unichar accumulator = 0;
    unichar octAccumulator = 0; /* required as we are also accumulating for
                                 * back ref when looking for octals */
    unsigned repeatN = 0;
    unsigned repeatM = 0;
    string label;

    ParseMode mode = globalMode;
    ParseMode newMode;

    bool negated = false;
    bool inComment = false;

    // Stack of sequences and flags used to store state when we enter
    // sub-sequences.
    vector<ExprState> sequences;

    // Index of the next capturing group. Note that zero is reserved for the
    // root sequence.
    unsigned groupIndex = 1;

    // Set storing group names that are currently in use.
    flat_set<string> groupNames;

    // Root sequence.
    unique_ptr<ComponentSequence> rootSeq = std::make_unique<ComponentSequence>();
    rootSeq->setCaptureIndex(0);

    // Current sequence being appended to
    ComponentSequence *currentSeq = rootSeq.get();

    // The current character class being appended to. This is used as the
    // accumulator for both character class and UCP properties.
    unique_ptr<ComponentClass> currentCls;

    // True if the machine is currently inside a character class, i.e. square
    // brackets [..].
    bool inCharClass = false;

    // True if the machine is inside a character class but it has not processed
    // any "real" elements yet, i.e. it's still processing meta-characters like
    // '^'.
    bool inCharClassEarly = false;

    // Location at which the current character class began.
    const char *currentClsBegin = p;

    // We throw exceptions on various parsing failures beyond this point: we
    // use a try/catch block here to clean up our allocated memory before we
    // re-throw the exception to the caller.
    try {
        // Embed the Ragel machine here
        
#line 811 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
	{
	cs = regex_start;
	top = 0;
	ts = 0;
	te = 0;
	act = 0;
	}

#line 1984 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
        
#line 818 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
	{
	const short *_acts;
	unsigned int _nacts;
	short _widec;

	if ( p == pe )
		goto _test_eof;
	if ( cs == 0 )
		goto _out;
_resume:
	_acts = _regex_actions + _regex_from_state_actions[cs];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 ) {
		switch ( *_acts++ ) {
	case 24:
#line 1 "NONE"
	{ts = p;}
	break;
#line 835 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
		}
	}

	switch ( cs ) {
case 746:
	_widec = (*p);
	if ( (*p) < 192u ) {
		if ( (*p) > 35u ) {
			if ( 128u <= (*p) && (*p) <= 191u ) {
				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 35u ) {
			_widec = (short)(1280u + ((*p) - 0u));
			if ( 
#line 477 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.ignore_space  ) _widec += 256;
		}
	} else if ( (*p) > 223u ) {
		if ( (*p) < 240u ) {
			if ( 224u <= (*p) && (*p) <= 239u ) {
				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) > 247u ) {
			if ( 248u <= (*p) )
 {				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	switch( _widec ) {
		case 0: goto tr873;
		case 32: goto tr875;
		case 36: goto tr876;
		case 40: goto tr878;
		case 41: goto tr879;
		case 42: goto tr880;
		case 43: goto tr881;
		case 46: goto tr882;
		case 63: goto tr883;
		case 91: goto tr884;
		case 92: goto tr885;
		case 94: goto tr886;
		case 123: goto tr887;
		case 124: goto tr888;
		case 1315: goto tr874;
		case 1571: goto tr893;
	}
	if ( _widec < 384 ) {
		if ( _widec < 9 ) {
			if ( 1 <= _widec && _widec <= 8 )
				goto tr874;
		} else if ( _widec > 13 ) {
			if ( _widec > 34 ) {
				if ( 37 <= _widec && _widec <= 127 )
					goto tr874;
			} else if ( _widec >= 14 )
				goto tr874;
		} else
			goto tr875;
	} else if ( _widec > 511 ) {
		if ( _widec < 736 ) {
			if ( _widec > 703 ) {
				if ( 704 <= _widec && _widec <= 735 )
					goto tr890;
			} else if ( _widec >= 640 )
				goto tr889;
		} else if ( _widec > 751 ) {
			if ( _widec > 759 ) {
				if ( 760 <= _widec && _widec <= 767 )
					goto tr889;
			} else if ( _widec >= 752 )
				goto tr892;
		} else
			goto tr891;
	} else
		goto tr874;
	goto tr877;
case 0:
	goto _out;
case 747:
	switch( (*p) ) {
		case 42u: goto tr895;
		case 63u: goto tr896;
	}
	goto tr894;
case 1:
	if ( (*p) == 41u )
		goto tr0;
	goto tr1;
case 2:
	switch( (*p) ) {
		case 33u: goto tr3;
		case 35u: goto tr4;
		case 38u: goto tr5;
		case 39u: goto tr6;
		case 40u: goto tr7;
		case 41u: goto tr8;
		case 43u: goto tr9;
		case 45u: goto tr10;
		case 58u: goto tr12;
		case 60u: goto tr13;
		case 61u: goto tr14;
		case 62u: goto tr15;
		case 63u: goto tr16;
		case 67u: goto tr17;
		case 80u: goto tr18;
		case 105u: goto tr19;
		case 109u: goto tr19;
		case 115u: goto tr19;
		case 120u: goto tr19;
		case 123u: goto tr20;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr11;
	goto tr2;
case 748:
	if ( (*p) == 95u )
		goto tr23;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr23;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr23;
	} else
		goto tr23;
	goto tr897;
case 3:
	switch( (*p) ) {
		case 41u: goto tr22;
		case 95u: goto tr23;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr23;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr23;
	} else
		goto tr23;
	goto tr21;
case 749:
	if ( (*p) == 95u )
		goto tr25;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr25;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr25;
	} else
		goto tr25;
	goto tr897;
case 4:
	switch( (*p) ) {
		case 39u: goto tr24;
		case 95u: goto tr25;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr25;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr25;
	} else
		goto tr25;
	goto tr21;
case 750:
	switch( (*p) ) {
		case 39u: goto tr899;
		case 48u: goto tr30;
		case 60u: goto tr902;
		case 63u: goto tr903;
		case 82u: goto tr904;
		case 95u: goto tr30;
	}
	if ( (*p) < 56u ) {
		if ( 49u <= (*p) && (*p) <= 55u )
			goto tr900;
	} else if ( (*p) > 57u ) {
		if ( (*p) > 90u ) {
			if ( 97u <= (*p) && (*p) <= 122u )
				goto tr30;
		} else if ( (*p) >= 65u )
			goto tr30;
	} else
		goto tr901;
	goto tr898;
case 5:
	if ( (*p) == 95u )
		goto tr27;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr27;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr27;
	} else
		goto tr27;
	goto tr26;
case 6:
	switch( (*p) ) {
		case 39u: goto tr28;
		case 95u: goto tr27;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr27;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr27;
	} else
		goto tr27;
	goto tr26;
case 7:
	if ( (*p) == 41u )
		goto tr29;
	goto tr26;
case 8:
	switch( (*p) ) {
		case 41u: goto tr29;
		case 95u: goto tr30;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr30;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr30;
	} else
		goto tr30;
	goto tr26;
case 9:
	switch( (*p) ) {
		case 41u: goto tr31;
		case 95u: goto tr30;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr32;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr30;
	} else
		goto tr30;
	goto tr26;
case 10:
	switch( (*p) ) {
		case 41u: goto tr29;
		case 95u: goto tr30;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr32;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr30;
	} else
		goto tr30;
	goto tr26;
case 11:
	if ( (*p) == 95u )
		goto tr33;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr33;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr33;
	} else
		goto tr33;
	goto tr26;
case 12:
	switch( (*p) ) {
		case 62u: goto tr28;
		case 95u: goto tr33;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr33;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr33;
	} else
		goto tr33;
	goto tr26;
case 13:
	switch( (*p) ) {
		case 33u: goto tr34;
		case 60u: goto tr35;
		case 61u: goto tr36;
	}
	goto tr26;
case 14:
	switch( (*p) ) {
		case 33u: goto tr37;
		case 61u: goto tr38;
	}
	goto tr26;
case 15:
	switch( (*p) ) {
		case 38u: goto tr39;
		case 41u: goto tr40;
		case 95u: goto tr30;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr41;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr30;
	} else
		goto tr30;
	goto tr26;
case 16:
	if ( (*p) == 95u )
		goto tr42;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr42;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr42;
	} else
		goto tr42;
	goto tr26;
case 17:
	switch( (*p) ) {
		case 41u: goto tr40;
		case 95u: goto tr42;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr42;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr42;
	} else
		goto tr42;
	goto tr26;
case 18:
	switch( (*p) ) {
		case 41u: goto tr40;
		case 95u: goto tr30;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr41;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr30;
	} else
		goto tr30;
	goto tr26;
case 751:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr44;
	goto tr897;
case 19:
	if ( (*p) == 41u )
		goto tr43;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr44;
	goto tr21;
case 752:
	switch( (*p) ) {
		case 105u: goto tr47;
		case 109u: goto tr47;
		case 115u: goto tr47;
		case 120u: goto tr47;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr44;
	goto tr897;
case 20:
	switch( (*p) ) {
		case 41u: goto tr45;
		case 58u: goto tr46;
		case 105u: goto tr47;
		case 109u: goto tr47;
		case 115u: goto tr47;
		case 120u: goto tr47;
	}
	goto tr21;
case 753:
	if ( (*p) == 41u )
		goto tr43;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr44;
	goto tr897;
case 754:
	switch( (*p) ) {
		case 33u: goto tr905;
		case 61u: goto tr906;
		case 95u: goto tr48;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr48;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr48;
	} else
		goto tr48;
	goto tr897;
case 21:
	switch( (*p) ) {
		case 62u: goto tr24;
		case 95u: goto tr48;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr48;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr48;
	} else
		goto tr48;
	goto tr21;
case 755:
	if ( (*p) == 123u )
		goto tr907;
	goto tr897;
case 756:
	if ( (*p) == 41u )
		goto tr49;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr50;
	goto tr897;
case 22:
	if ( (*p) == 41u )
		goto tr49;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr50;
	goto tr21;
case 757:
	switch( (*p) ) {
		case 60u: goto tr908;
		case 61u: goto tr909;
		case 62u: goto tr910;
	}
	goto tr897;
case 23:
	if ( (*p) == 95u )
		goto tr48;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr48;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr48;
	} else
		goto tr48;
	goto tr21;
case 24:
	if ( (*p) == 95u )
		goto tr51;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr51;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr51;
	} else
		goto tr51;
	goto tr21;
case 25:
	switch( (*p) ) {
		case 41u: goto tr52;
		case 95u: goto tr51;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr51;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr51;
	} else
		goto tr51;
	goto tr21;
case 26:
	if ( (*p) == 95u )
		goto tr23;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr23;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr23;
	} else
		goto tr23;
	goto tr21;
case 758:
	switch( (*p) ) {
		case 41u: goto tr45;
		case 45u: goto tr53;
		case 58u: goto tr46;
		case 105u: goto tr54;
		case 109u: goto tr54;
		case 115u: goto tr54;
		case 120u: goto tr54;
	}
	goto tr897;
case 27:
	switch( (*p) ) {
		case 105u: goto tr47;
		case 109u: goto tr47;
		case 115u: goto tr47;
		case 120u: goto tr47;
	}
	goto tr21;
case 28:
	switch( (*p) ) {
		case 41u: goto tr45;
		case 45u: goto tr53;
		case 58u: goto tr46;
		case 105u: goto tr54;
		case 109u: goto tr54;
		case 115u: goto tr54;
		case 120u: goto tr54;
	}
	goto tr21;
case 759:
	switch( (*p) ) {
		case 43u: goto tr912;
		case 63u: goto tr913;
	}
	goto tr911;
case 760:
	switch( (*p) ) {
		case 43u: goto tr915;
		case 63u: goto tr916;
	}
	goto tr914;
case 761:
	switch( (*p) ) {
		case 43u: goto tr918;
		case 63u: goto tr919;
	}
	goto tr917;
case 762:
	switch( (*p) ) {
		case 46u: goto tr56;
		case 58u: goto tr60;
		case 61u: goto tr64;
	}
	goto tr920;
case 29:
	switch( (*p) ) {
		case 46u: goto tr57;
		case 92u: goto tr58;
		case 93u: goto tr55;
	}
	goto tr56;
case 30:
	switch( (*p) ) {
		case 46u: goto tr57;
		case 92u: goto tr58;
		case 93u: goto tr59;
	}
	goto tr56;
case 31:
	switch( (*p) ) {
		case 46u: goto tr57;
		case 92u: goto tr58;
	}
	goto tr56;
case 32:
	switch( (*p) ) {
		case 58u: goto tr61;
		case 92u: goto tr62;
		case 93u: goto tr55;
	}
	goto tr60;
case 33:
	switch( (*p) ) {
		case 58u: goto tr61;
		case 92u: goto tr62;
		case 93u: goto tr63;
	}
	goto tr60;
case 34:
	switch( (*p) ) {
		case 58u: goto tr61;
		case 92u: goto tr62;
	}
	goto tr60;
case 35:
	switch( (*p) ) {
		case 61u: goto tr65;
		case 92u: goto tr66;
		case 93u: goto tr55;
	}
	goto tr64;
case 36:
	switch( (*p) ) {
		case 61u: goto tr65;
		case 92u: goto tr66;
		case 93u: goto tr59;
	}
	goto tr64;
case 37:
	switch( (*p) ) {
		case 61u: goto tr65;
		case 92u: goto tr66;
	}
	goto tr64;
case 763:
	switch( (*p) ) {
		case 48u: goto tr923;
		case 65u: goto tr926;
		case 66u: goto tr927;
		case 67u: goto tr928;
		case 68u: goto tr929;
		case 69u: goto tr930;
		case 71u: goto tr931;
		case 72u: goto tr932;
		case 75u: goto tr933;
		case 76u: goto tr934;
		case 78u: goto tr934;
		case 80u: goto tr935;
		case 81u: goto tr936;
		case 82u: goto tr937;
		case 83u: goto tr938;
		case 85u: goto tr934;
		case 86u: goto tr939;
		case 87u: goto tr940;
		case 88u: goto tr941;
		case 90u: goto tr942;
		case 97u: goto tr943;
		case 98u: goto tr944;
		case 99u: goto tr945;
		case 100u: goto tr946;
		case 101u: goto tr947;
		case 102u: goto tr948;
		case 103u: goto tr949;
		case 104u: goto tr950;
		case 107u: goto tr951;
		case 108u: goto tr934;
		case 110u: goto tr952;
		case 111u: goto tr953;
		case 112u: goto tr954;
		case 114u: goto tr955;
		case 115u: goto tr956;
		case 116u: goto tr957;
		case 117u: goto tr934;
		case 118u: goto tr958;
		case 119u: goto tr959;
		case 120u: goto tr960;
		case 122u: goto tr961;
	}
	if ( (*p) > 55u ) {
		if ( 56u <= (*p) && (*p) <= 57u )
			goto tr925;
	} else if ( (*p) >= 49u )
		goto tr924;
	goto tr922;
case 764:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr963;
	goto tr962;
case 765:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr964;
	goto tr962;
case 766:
	if ( (*p) > 55u ) {
		if ( 56u <= (*p) && (*p) <= 57u )
			goto tr967;
	} else if ( (*p) >= 48u )
		goto tr966;
	goto tr965;
case 767:
	if ( (*p) > 55u ) {
		if ( 56u <= (*p) && (*p) <= 57u )
			goto tr967;
	} else if ( (*p) >= 48u )
		goto tr969;
	goto tr968;
case 768:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr967;
	goto tr970;
case 769:
	if ( (*p) == 123u )
		goto tr973;
	goto tr972;
case 770:
	goto tr975;
case 771:
	switch( (*p) ) {
		case 39u: goto tr68;
		case 45u: goto tr977;
		case 60u: goto tr71;
		case 123u: goto tr979;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr978;
	goto tr976;
case 38:
	if ( (*p) == 39u )
		goto tr69;
	goto tr68;
case 39:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr70;
	goto tr67;
case 772:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr981;
	goto tr980;
case 773:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr982;
	goto tr980;
case 774:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr984;
	goto tr983;
case 775:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr985;
	goto tr983;
case 40:
	if ( (*p) == 62u )
		goto tr69;
	goto tr71;
case 41:
	switch( (*p) ) {
		case 45u: goto tr72;
		case 95u: goto tr74;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr73;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr74;
	} else
		goto tr74;
	goto tr67;
case 42:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr75;
	goto tr67;
case 43:
	if ( (*p) == 125u )
		goto tr77;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr76;
	goto tr67;
case 44:
	if ( (*p) == 125u )
		goto tr77;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr78;
	goto tr67;
case 45:
	if ( (*p) == 125u )
		goto tr77;
	goto tr67;
case 46:
	switch( (*p) ) {
		case 95u: goto tr74;
		case 125u: goto tr80;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr79;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr74;
	} else
		goto tr74;
	goto tr67;
case 47:
	switch( (*p) ) {
		case 95u: goto tr74;
		case 125u: goto tr80;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr81;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr74;
	} else
		goto tr74;
	goto tr67;
case 48:
	switch( (*p) ) {
		case 95u: goto tr74;
		case 125u: goto tr80;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr74;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr74;
	} else
		goto tr74;
	goto tr67;
case 49:
	switch( (*p) ) {
		case 95u: goto tr74;
		case 125u: goto tr82;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr74;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr74;
	} else
		goto tr74;
	goto tr67;
case 776:
	switch( (*p) ) {
		case 39u: goto tr987;
		case 60u: goto tr988;
		case 123u: goto tr989;
	}
	goto tr986;
case 50:
	if ( (*p) == 95u )
		goto tr84;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr84;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr84;
	} else
		goto tr84;
	goto tr83;
case 51:
	switch( (*p) ) {
		case 39u: goto tr85;
		case 95u: goto tr84;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr84;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr84;
	} else
		goto tr84;
	goto tr83;
case 52:
	if ( (*p) == 95u )
		goto tr86;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr86;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr86;
	} else
		goto tr86;
	goto tr83;
case 53:
	switch( (*p) ) {
		case 62u: goto tr87;
		case 95u: goto tr86;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr86;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr86;
	} else
		goto tr86;
	goto tr83;
case 54:
	if ( (*p) == 95u )
		goto tr88;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr88;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr88;
	} else
		goto tr88;
	goto tr83;
case 55:
	switch( (*p) ) {
		case 95u: goto tr88;
		case 125u: goto tr89;
	}
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr88;
	} else if ( (*p) > 90u ) {
		if ( 97u <= (*p) && (*p) <= 122u )
			goto tr88;
	} else
		goto tr88;
	goto tr83;
case 777:
	if ( (*p) == 123u )
		goto tr991;
	goto tr990;
case 56:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr91;
	goto tr90;
case 57:
	if ( (*p) == 125u )
		goto tr92;
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr91;
	goto tr90;
case 778:
	if ( (*p) == 123u )
		goto tr994;
	goto tr993;
case 779:
	if ( (*p) == 123u )
		goto tr999;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr996;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr998;
	} else
		goto tr997;
	goto tr995;
case 780:
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr1000;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr1002;
	} else
		goto tr1001;
	goto tr995;
case 781:
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr94;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr94;
	} else
		goto tr94;
	goto tr1003;
case 58:
	if ( (*p) == 125u )
		goto tr95;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr94;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr94;
	} else
		goto tr94;
	goto tr93;
case 782:
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr98;
	goto tr1004;
case 59:
	switch( (*p) ) {
		case 44u: goto tr97;
		case 125u: goto tr99;
	}
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr98;
	goto tr96;
case 60:
	if ( (*p) == 125u )
		goto tr101;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr100;
	goto tr96;
case 61:
	if ( (*p) == 125u )
		goto tr102;
	if ( 48u <= (*p) && (*p) <= 57u )
		goto tr100;
	goto tr96;
case 783:
	switch( (*p) ) {
		case 43u: goto tr1006;
		case 63u: goto tr1007;
	}
	goto tr1005;
case 784:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1009;
	goto tr1008;
case 785:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1010;
	goto tr1008;
case 62:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr104;
	goto tr103;
case 786:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1011;
	goto tr1008;
case 63:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr105;
	goto tr103;
case 64:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr106;
	goto tr103;
case 559:
	if ( (*p) == 123u )
		goto tr650;
	goto tr649;
case 560:
	if ( (*p) == 94u )
		goto tr652;
	goto tr651;
case 561:
	if ( (*p) == 125u )
		goto tr653;
	goto tr649;
case 817:
	goto tr649;
case 562:
	if ( (*p) == 94u )
		goto tr649;
	goto tr651;
case 787:
	switch( (*p) ) {
		case 41u: goto tr1013;
		case 85u: goto tr1014;
	}
	goto tr1012;
case 788:
	if ( (*p) == 41u )
		goto tr109;
	goto tr108;
case 65:
	if ( (*p) == 41u )
		goto tr109;
	goto tr108;
case 789:
	switch( (*p) ) {
		case 41u: goto tr109;
		case 67u: goto tr1016;
		case 84u: goto tr1017;
	}
	goto tr108;
case 66:
	switch( (*p) ) {
		case 41u: goto tr109;
		case 80u: goto tr110;
	}
	goto tr108;
case 67:
	if ( (*p) == 41u )
		goto tr111;
	goto tr108;
case 68:
	switch( (*p) ) {
		case 41u: goto tr109;
		case 70u: goto tr112;
	}
	goto tr108;
case 69:
	switch( (*p) ) {
		case 41u: goto tr113;
		case 56u: goto tr114;
	}
	goto tr108;
case 70:
	if ( (*p) == 41u )
		goto tr115;
	goto tr108;
case 790:
	switch( (*p) ) {
		case 65u: goto tr1019;
		case 66u: goto tr1020;
		case 67u: goto tr1021;
		case 68u: goto tr1022;
		case 69u: goto tr1023;
		case 71u: goto tr1024;
		case 72u: goto tr1025;
		case 73u: goto tr1026;
		case 74u: goto tr1027;
		case 75u: goto tr1028;
		case 76u: goto tr1029;
		case 77u: goto tr1030;
		case 78u: goto tr1031;
		case 79u: goto tr1032;
		case 80u: goto tr1033;
		case 82u: goto tr1034;
		case 83u: goto tr1035;
		case 84u: goto tr1036;
		case 85u: goto tr1037;
		case 86u: goto tr1038;
		case 88u: goto tr1039;
		case 89u: goto tr1040;
		case 90u: goto tr1041;
	}
	goto tr1018;
case 791:
	switch( (*p) ) {
		case 110u: goto tr1043;
		case 114u: goto tr1044;
		case 118u: goto tr1045;
	}
	goto tr1042;
case 71:
	if ( (*p) == 121u )
		goto tr117;
	goto tr116;
case 72:
	switch( (*p) ) {
		case 97u: goto tr118;
		case 109u: goto tr119;
	}
	goto tr116;
case 73:
	if ( (*p) == 98u )
		goto tr120;
	goto tr116;
case 74:
	if ( (*p) == 105u )
		goto tr121;
	goto tr116;
case 75:
	if ( (*p) == 99u )
		goto tr122;
	goto tr116;
case 76:
	if ( (*p) == 101u )
		goto tr123;
	goto tr116;
case 77:
	if ( (*p) == 110u )
		goto tr124;
	goto tr116;
case 78:
	if ( (*p) == 105u )
		goto tr125;
	goto tr116;
case 79:
	if ( (*p) == 97u )
		goto tr126;
	goto tr116;
case 80:
	if ( (*p) == 110u )
		goto tr127;
	goto tr116;
case 81:
	if ( (*p) == 101u )
		goto tr128;
	goto tr116;
case 82:
	if ( (*p) == 115u )
		goto tr129;
	goto tr116;
case 83:
	if ( (*p) == 116u )
		goto tr130;
	goto tr116;
case 84:
	if ( (*p) == 97u )
		goto tr131;
	goto tr116;
case 85:
	if ( (*p) == 110u )
		goto tr132;
	goto tr116;
case 792:
	switch( (*p) ) {
		case 97u: goto tr1046;
		case 101u: goto tr1047;
		case 111u: goto tr1048;
		case 114u: goto tr1049;
		case 117u: goto tr1050;
	}
	goto tr1042;
case 86:
	switch( (*p) ) {
		case 108u: goto tr133;
		case 109u: goto tr134;
		case 116u: goto tr135;
	}
	goto tr116;
case 87:
	if ( (*p) == 105u )
		goto tr136;
	goto tr116;
case 88:
	if ( (*p) == 110u )
		goto tr137;
	goto tr116;
case 89:
	if ( (*p) == 101u )
		goto tr138;
	goto tr116;
case 90:
	if ( (*p) == 115u )
		goto tr139;
	goto tr116;
case 91:
	if ( (*p) == 101u )
		goto tr140;
	goto tr116;
case 92:
	if ( (*p) == 117u )
		goto tr141;
	goto tr116;
case 93:
	if ( (*p) == 109u )
		goto tr142;
	goto tr116;
case 94:
	if ( (*p) == 97u )
		goto tr143;
	goto tr116;
case 95:
	if ( (*p) == 107u )
		goto tr144;
	goto tr116;
case 96:
	if ( (*p) == 110u )
		goto tr145;
	goto tr116;
case 97:
	if ( (*p) == 103u )
		goto tr146;
	goto tr116;
case 98:
	if ( (*p) == 97u )
		goto tr147;
	goto tr116;
case 99:
	if ( (*p) == 108u )
		goto tr148;
	goto tr116;
case 100:
	if ( (*p) == 105u )
		goto tr149;
	goto tr116;
case 101:
	if ( (*p) == 112u )
		goto tr150;
	goto tr116;
case 102:
	if ( (*p) == 111u )
		goto tr151;
	goto tr116;
case 103:
	if ( (*p) == 109u )
		goto tr152;
	goto tr116;
case 104:
	if ( (*p) == 111u )
		goto tr153;
	goto tr116;
case 105:
	if ( (*p) == 102u )
		goto tr154;
	goto tr116;
case 106:
	if ( (*p) == 111u )
		goto tr155;
	goto tr116;
case 107:
	if ( (*p) == 97u )
		goto tr156;
	goto tr116;
case 108:
	switch( (*p) ) {
		case 104u: goto tr157;
		case 105u: goto tr158;
	}
	goto tr116;
case 109:
	if ( (*p) == 109u )
		goto tr159;
	goto tr116;
case 110:
	if ( (*p) == 105u )
		goto tr160;
	goto tr116;
case 111:
	if ( (*p) == 108u )
		goto tr161;
	goto tr116;
case 112:
	if ( (*p) == 108u )
		goto tr162;
	goto tr116;
case 113:
	if ( (*p) == 101u )
		goto tr163;
	goto tr116;
case 114:
	switch( (*p) ) {
		case 103u: goto tr164;
		case 104u: goto tr165;
	}
	goto tr116;
case 115:
	if ( (*p) == 105u )
		goto tr166;
	goto tr116;
case 116:
	if ( (*p) == 110u )
		goto tr167;
	goto tr116;
case 117:
	if ( (*p) == 101u )
		goto tr168;
	goto tr116;
case 118:
	if ( (*p) == 115u )
		goto tr169;
	goto tr116;
case 119:
	if ( (*p) == 101u )
		goto tr170;
	goto tr116;
case 120:
	if ( (*p) == 105u )
		goto tr171;
	goto tr116;
case 121:
	if ( (*p) == 100u )
		goto tr172;
	goto tr116;
case 793:
	switch( (*p) ) {
		case 97u: goto tr1052;
		case 99u: goto tr1053;
		case 102u: goto tr1054;
		case 104u: goto tr1055;
		case 110u: goto tr1056;
		case 111u: goto tr1057;
		case 115u: goto tr1058;
		case 117u: goto tr1059;
		case 121u: goto tr1060;
	}
	goto tr1051;
case 122:
	switch( (*p) ) {
		case 110u: goto tr174;
		case 114u: goto tr175;
	}
	goto tr173;
case 123:
	if ( (*p) == 97u )
		goto tr176;
	goto tr173;
case 124:
	if ( (*p) == 100u )
		goto tr177;
	goto tr173;
case 125:
	if ( (*p) == 105u )
		goto tr178;
	goto tr173;
case 126:
	if ( (*p) == 97u )
		goto tr179;
	goto tr173;
case 127:
	if ( (*p) == 110u )
		goto tr180;
	goto tr173;
case 128:
	if ( (*p) == 95u )
		goto tr181;
	goto tr173;
case 129:
	if ( (*p) == 65u )
		goto tr182;
	goto tr173;
case 130:
	if ( (*p) == 98u )
		goto tr183;
	goto tr173;
case 131:
	if ( (*p) == 111u )
		goto tr184;
	goto tr173;
case 132:
	if ( (*p) == 114u )
		goto tr185;
	goto tr173;
case 133:
	if ( (*p) == 105u )
		goto tr186;
	goto tr173;
case 134:
	if ( (*p) == 103u )
		goto tr187;
	goto tr173;
case 135:
	if ( (*p) == 105u )
		goto tr188;
	goto tr173;
case 136:
	if ( (*p) == 110u )
		goto tr189;
	goto tr173;
case 137:
	if ( (*p) == 97u )
		goto tr190;
	goto tr173;
case 138:
	if ( (*p) == 108u )
		goto tr191;
	goto tr173;
case 139:
	if ( (*p) == 105u )
		goto tr192;
	goto tr173;
case 140:
	if ( (*p) == 97u )
		goto tr193;
	goto tr173;
case 141:
	if ( (*p) == 110u )
		goto tr194;
	goto tr173;
case 142:
	switch( (*p) ) {
		case 97u: goto tr195;
		case 101u: goto tr196;
	}
	goto tr173;
case 143:
	if ( (*p) == 109u )
		goto tr197;
	goto tr173;
case 144:
	if ( (*p) == 114u )
		goto tr198;
	goto tr173;
case 145:
	if ( (*p) == 111u )
		goto tr199;
	goto tr173;
case 146:
	if ( (*p) == 107u )
		goto tr200;
	goto tr173;
case 147:
	if ( (*p) == 101u )
		goto tr201;
	goto tr173;
case 148:
	if ( (*p) == 101u )
		goto tr202;
	goto tr173;
case 794:
	switch( (*p) ) {
		case 109u: goto tr1062;
		case 112u: goto tr1063;
	}
	goto tr1061;
case 149:
	if ( (*p) == 109u )
		goto tr204;
	goto tr203;
case 150:
	if ( (*p) == 111u )
		goto tr205;
	goto tr203;
case 151:
	if ( (*p) == 110u )
		goto tr206;
	goto tr203;
case 152:
	if ( (*p) == 116u )
		goto tr207;
	goto tr203;
case 153:
	if ( (*p) == 105u )
		goto tr208;
	goto tr203;
case 154:
	if ( (*p) == 99u )
		goto tr209;
	goto tr203;
case 155:
	if ( (*p) == 110u )
		goto tr210;
	goto tr173;
case 156:
	if ( (*p) == 101u )
		goto tr211;
	goto tr173;
case 157:
	if ( (*p) == 105u )
		goto tr212;
	goto tr173;
case 158:
	if ( (*p) == 102u )
		goto tr213;
	goto tr173;
case 159:
	if ( (*p) == 111u )
		goto tr214;
	goto tr173;
case 160:
	if ( (*p) == 114u )
		goto tr215;
	goto tr173;
case 161:
	if ( (*p) == 109u )
		goto tr216;
	goto tr173;
case 162:
	switch( (*p) ) {
		case 112u: goto tr217;
		case 114u: goto tr218;
	}
	goto tr173;
case 163:
	if ( (*p) == 114u )
		goto tr219;
	goto tr173;
case 164:
	if ( (*p) == 105u )
		goto tr220;
	goto tr173;
case 165:
	if ( (*p) == 111u )
		goto tr221;
	goto tr173;
case 166:
	if ( (*p) == 116u )
		goto tr222;
	goto tr173;
case 167:
	if ( (*p) == 105u )
		goto tr223;
	goto tr173;
case 168:
	if ( (*p) == 108u )
		goto tr224;
	goto tr173;
case 169:
	if ( (*p) == 108u )
		goto tr225;
	goto tr173;
case 170:
	if ( (*p) == 105u )
		goto tr226;
	goto tr173;
case 171:
	if ( (*p) == 99u )
		goto tr227;
	goto tr173;
case 795:
	if ( (*p) == 101u )
		goto tr1064;
	goto tr1042;
case 172:
	switch( (*p) ) {
		case 115u: goto tr228;
		case 118u: goto tr229;
	}
	goto tr116;
case 173:
	if ( (*p) == 101u )
		goto tr230;
	goto tr116;
case 174:
	if ( (*p) == 114u )
		goto tr231;
	goto tr116;
case 175:
	if ( (*p) == 101u )
		goto tr232;
	goto tr116;
case 176:
	if ( (*p) == 116u )
		goto tr233;
	goto tr116;
case 177:
	if ( (*p) == 97u )
		goto tr234;
	goto tr116;
case 178:
	if ( (*p) == 110u )
		goto tr235;
	goto tr116;
case 179:
	if ( (*p) == 97u )
		goto tr236;
	goto tr116;
case 180:
	if ( (*p) == 103u )
		goto tr237;
	goto tr116;
case 181:
	if ( (*p) == 97u )
		goto tr238;
	goto tr116;
case 182:
	if ( (*p) == 114u )
		goto tr239;
	goto tr116;
case 183:
	if ( (*p) == 105u )
		goto tr240;
	goto tr116;
case 796:
	switch( (*p) ) {
		case 103u: goto tr1065;
		case 116u: goto tr1066;
	}
	goto tr1042;
case 184:
	if ( (*p) == 121u )
		goto tr241;
	goto tr116;
case 185:
	if ( (*p) == 112u )
		goto tr242;
	goto tr116;
case 186:
	if ( (*p) == 116u )
		goto tr243;
	goto tr116;
case 187:
	if ( (*p) == 105u )
		goto tr244;
	goto tr116;
case 188:
	if ( (*p) == 97u )
		goto tr245;
	goto tr116;
case 189:
	if ( (*p) == 110u )
		goto tr246;
	goto tr116;
case 190:
	if ( (*p) == 95u )
		goto tr247;
	goto tr116;
case 191:
	if ( (*p) == 72u )
		goto tr248;
	goto tr116;
case 192:
	if ( (*p) == 105u )
		goto tr249;
	goto tr116;
case 193:
	if ( (*p) == 101u )
		goto tr250;
	goto tr116;
case 194:
	if ( (*p) == 114u )
		goto tr251;
	goto tr116;
case 195:
	if ( (*p) == 111u )
		goto tr252;
	goto tr116;
case 196:
	if ( (*p) == 103u )
		goto tr253;
	goto tr116;
case 197:
	if ( (*p) == 108u )
		goto tr254;
	goto tr116;
case 198:
	if ( (*p) == 121u )
		goto tr255;
	goto tr116;
case 199:
	if ( (*p) == 112u )
		goto tr256;
	goto tr116;
case 200:
	if ( (*p) == 104u )
		goto tr257;
	goto tr116;
case 201:
	if ( (*p) == 115u )
		goto tr258;
	goto tr116;
case 202:
	if ( (*p) == 104u )
		goto tr259;
	goto tr116;
case 203:
	if ( (*p) == 105u )
		goto tr260;
	goto tr116;
case 204:
	if ( (*p) == 111u )
		goto tr261;
	goto tr116;
case 205:
	if ( (*p) == 112u )
		goto tr262;
	goto tr116;
case 206:
	if ( (*p) == 105u )
		goto tr263;
	goto tr116;
case 207:
	if ( (*p) == 99u )
		goto tr264;
	goto tr116;
case 797:
	switch( (*p) ) {
		case 101u: goto tr1067;
		case 108u: goto tr1068;
		case 111u: goto tr1069;
		case 114u: goto tr1070;
		case 117u: goto tr1071;
	}
	goto tr1042;
case 208:
	if ( (*p) == 111u )
		goto tr265;
	goto tr116;
case 209:
	if ( (*p) == 114u )
		goto tr266;
	goto tr116;
case 210:
	if ( (*p) == 103u )
		goto tr267;
	goto tr116;
case 211:
	if ( (*p) == 105u )
		goto tr268;
	goto tr116;
case 212:
	if ( (*p) == 97u )
		goto tr269;
	goto tr116;
case 213:
	if ( (*p) == 110u )
		goto tr270;
	goto tr116;
case 214:
	if ( (*p) == 97u )
		goto tr271;
	goto tr116;
case 215:
	if ( (*p) == 103u )
		goto tr272;
	goto tr116;
case 216:
	if ( (*p) == 111u )
		goto tr273;
	goto tr116;
case 217:
	if ( (*p) == 108u )
		goto tr274;
	goto tr116;
case 218:
	if ( (*p) == 105u )
		goto tr275;
	goto tr116;
case 219:
	if ( (*p) == 116u )
		goto tr276;
	goto tr116;
case 220:
	if ( (*p) == 105u )
		goto tr277;
	goto tr116;
case 221:
	if ( (*p) == 99u )
		goto tr278;
	goto tr116;
case 222:
	if ( (*p) == 116u )
		goto tr279;
	goto tr116;
case 223:
	if ( (*p) == 104u )
		goto tr280;
	goto tr116;
case 224:
	if ( (*p) == 105u )
		goto tr281;
	goto tr116;
case 225:
	if ( (*p) == 99u )
		goto tr282;
	goto tr116;
case 226:
	if ( (*p) == 101u )
		goto tr283;
	goto tr116;
case 227:
	if ( (*p) == 101u )
		goto tr284;
	goto tr116;
case 228:
	if ( (*p) == 107u )
		goto tr285;
	goto tr116;
case 229:
	switch( (*p) ) {
		case 106u: goto tr286;
		case 114u: goto tr287;
	}
	goto tr116;
case 230:
	if ( (*p) == 97u )
		goto tr288;
	goto tr116;
case 231:
	if ( (*p) == 114u )
		goto tr289;
	goto tr116;
case 232:
	if ( (*p) == 97u )
		goto tr290;
	goto tr116;
case 233:
	if ( (*p) == 116u )
		goto tr291;
	goto tr116;
case 234:
	if ( (*p) == 105u )
		goto tr292;
	goto tr116;
case 235:
	if ( (*p) == 109u )
		goto tr293;
	goto tr116;
case 236:
	if ( (*p) == 117u )
		goto tr294;
	goto tr116;
case 237:
	if ( (*p) == 107u )
		goto tr295;
	goto tr116;
case 238:
	if ( (*p) == 104u )
		goto tr296;
	goto tr116;
case 239:
	if ( (*p) == 105u )
		goto tr297;
	goto tr116;
case 798:
	switch( (*p) ) {
		case 97u: goto tr1072;
		case 101u: goto tr1073;
		case 105u: goto tr1074;
	}
	goto tr1042;
case 240:
	if ( (*p) == 110u )
		goto tr298;
	goto tr116;
case 799:
	switch( (*p) ) {
		case 103u: goto tr1076;
		case 117u: goto tr1077;
	}
	goto tr1075;
case 241:
	if ( (*p) == 117u )
		goto tr300;
	goto tr299;
case 242:
	if ( (*p) == 108u )
		goto tr301;
	goto tr299;
case 243:
	if ( (*p) == 110u )
		goto tr302;
	goto tr299;
case 244:
	if ( (*p) == 111u )
		goto tr303;
	goto tr299;
case 245:
	if ( (*p) == 111u )
		goto tr304;
	goto tr299;
case 246:
	if ( (*p) == 98u )
		goto tr305;
	goto tr116;
case 247:
	if ( (*p) == 114u )
		goto tr306;
	goto tr116;
case 248:
	if ( (*p) == 101u )
		goto tr307;
	goto tr116;
case 249:
	if ( (*p) == 119u )
		goto tr308;
	goto tr116;
case 250:
	if ( (*p) == 114u )
		goto tr309;
	goto tr116;
case 251:
	if ( (*p) == 97u )
		goto tr310;
	goto tr116;
case 252:
	if ( (*p) == 103u )
		goto tr311;
	goto tr116;
case 253:
	if ( (*p) == 97u )
		goto tr312;
	goto tr116;
case 254:
	if ( (*p) == 110u )
		goto tr313;
	goto tr116;
case 255:
	if ( (*p) == 97u )
		goto tr314;
	goto tr116;
case 800:
	switch( (*p) ) {
		case 109u: goto tr1078;
		case 110u: goto tr1079;
	}
	goto tr1042;
case 256:
	if ( (*p) == 112u )
		goto tr315;
	goto tr116;
case 257:
	if ( (*p) == 101u )
		goto tr316;
	goto tr116;
case 258:
	if ( (*p) == 114u )
		goto tr317;
	goto tr116;
case 259:
	if ( (*p) == 105u )
		goto tr318;
	goto tr116;
case 260:
	if ( (*p) == 97u )
		goto tr319;
	goto tr116;
case 261:
	if ( (*p) == 108u )
		goto tr320;
	goto tr116;
case 262:
	if ( (*p) == 95u )
		goto tr321;
	goto tr116;
case 263:
	if ( (*p) == 65u )
		goto tr322;
	goto tr116;
case 264:
	if ( (*p) == 114u )
		goto tr323;
	goto tr116;
case 265:
	if ( (*p) == 97u )
		goto tr324;
	goto tr116;
case 266:
	if ( (*p) == 109u )
		goto tr325;
	goto tr116;
case 267:
	if ( (*p) == 97u )
		goto tr326;
	goto tr116;
case 268:
	if ( (*p) == 105u )
		goto tr327;
	goto tr116;
case 269:
	if ( (*p) == 99u )
		goto tr328;
	goto tr116;
case 270:
	switch( (*p) ) {
		case 104u: goto tr329;
		case 115u: goto tr330;
	}
	goto tr116;
case 271:
	if ( (*p) == 101u )
		goto tr331;
	goto tr116;
case 272:
	if ( (*p) == 114u )
		goto tr332;
	goto tr116;
case 273:
	if ( (*p) == 105u )
		goto tr333;
	goto tr116;
case 274:
	if ( (*p) == 116u )
		goto tr334;
	goto tr116;
case 275:
	if ( (*p) == 101u )
		goto tr335;
	goto tr116;
case 276:
	if ( (*p) == 100u )
		goto tr336;
	goto tr116;
case 277:
	if ( (*p) == 99u )
		goto tr337;
	goto tr116;
case 278:
	if ( (*p) == 114u )
		goto tr338;
	goto tr116;
case 279:
	if ( (*p) == 105u )
		goto tr339;
	goto tr116;
case 280:
	if ( (*p) == 112u )
		goto tr340;
	goto tr116;
case 281:
	if ( (*p) == 116u )
		goto tr341;
	goto tr116;
case 282:
	if ( (*p) == 105u )
		goto tr342;
	goto tr116;
case 283:
	if ( (*p) == 111u )
		goto tr343;
	goto tr116;
case 284:
	if ( (*p) == 110u )
		goto tr344;
	goto tr116;
case 285:
	if ( (*p) == 97u )
		goto tr345;
	goto tr116;
case 286:
	if ( (*p) == 108u )
		goto tr346;
	goto tr116;
case 287:
	if ( (*p) == 95u )
		goto tr347;
	goto tr116;
case 288:
	if ( (*p) == 80u )
		goto tr348;
	goto tr116;
case 289:
	if ( (*p) == 97u )
		goto tr349;
	goto tr116;
case 290:
	switch( (*p) ) {
		case 104u: goto tr350;
		case 114u: goto tr351;
	}
	goto tr116;
case 291:
	if ( (*p) == 108u )
		goto tr352;
	goto tr116;
case 292:
	if ( (*p) == 97u )
		goto tr353;
	goto tr116;
case 293:
	if ( (*p) == 118u )
		goto tr354;
	goto tr116;
case 294:
	if ( (*p) == 105u )
		goto tr355;
	goto tr116;
case 295:
	if ( (*p) == 116u )
		goto tr356;
	goto tr116;
case 296:
	if ( (*p) == 104u )
		goto tr357;
	goto tr116;
case 297:
	if ( (*p) == 105u )
		goto tr358;
	goto tr116;
case 298:
	if ( (*p) == 97u )
		goto tr359;
	goto tr116;
case 299:
	if ( (*p) == 110u )
		goto tr360;
	goto tr116;
case 801:
	if ( (*p) == 97u )
		goto tr1080;
	goto tr1042;
case 300:
	if ( (*p) == 118u )
		goto tr361;
	goto tr116;
case 301:
	if ( (*p) == 97u )
		goto tr362;
	goto tr116;
case 302:
	if ( (*p) == 110u )
		goto tr363;
	goto tr116;
case 303:
	if ( (*p) == 101u )
		goto tr364;
	goto tr116;
case 304:
	if ( (*p) == 115u )
		goto tr365;
	goto tr116;
case 305:
	if ( (*p) == 101u )
		goto tr366;
	goto tr116;
case 802:
	switch( (*p) ) {
		case 97u: goto tr1081;
		case 104u: goto tr1082;
	}
	goto tr1042;
case 306:
	switch( (*p) ) {
		case 105u: goto tr367;
		case 110u: goto tr368;
		case 116u: goto tr369;
		case 121u: goto tr370;
	}
	goto tr116;
case 307:
	if ( (*p) == 116u )
		goto tr371;
	goto tr116;
case 308:
	if ( (*p) == 104u )
		goto tr372;
	goto tr116;
case 309:
	if ( (*p) == 105u )
		goto tr373;
	goto tr116;
case 310:
	if ( (*p) == 110u )
		goto tr374;
	goto tr116;
case 311:
	if ( (*p) == 97u )
		goto tr375;
	goto tr116;
case 312:
	if ( (*p) == 100u )
		goto tr376;
	goto tr116;
case 313:
	if ( (*p) == 97u )
		goto tr377;
	goto tr116;
case 314:
	if ( (*p) == 97u )
		goto tr378;
	goto tr116;
case 315:
	if ( (*p) == 107u )
		goto tr379;
	goto tr116;
case 316:
	if ( (*p) == 97u )
		goto tr380;
	goto tr116;
case 317:
	if ( (*p) == 110u )
		goto tr381;
	goto tr116;
case 318:
	if ( (*p) == 97u )
		goto tr382;
	goto tr116;
case 319:
	if ( (*p) == 97u )
		goto tr383;
	goto tr116;
case 320:
	if ( (*p) == 104u )
		goto tr384;
	goto tr116;
case 321:
	if ( (*p) == 95u )
		goto tr385;
	goto tr116;
case 322:
	if ( (*p) == 76u )
		goto tr386;
	goto tr116;
case 323:
	if ( (*p) == 105u )
		goto tr387;
	goto tr116;
case 324:
	switch( (*p) ) {
		case 97u: goto tr388;
		case 109u: goto tr389;
	}
	goto tr116;
case 325:
	if ( (*p) == 114u )
		goto tr390;
	goto tr116;
case 326:
	if ( (*p) == 111u )
		goto tr391;
	goto tr116;
case 327:
	if ( (*p) == 115u )
		goto tr392;
	goto tr116;
case 328:
	if ( (*p) == 104u )
		goto tr393;
	goto tr116;
case 329:
	if ( (*p) == 116u )
		goto tr394;
	goto tr116;
case 330:
	if ( (*p) == 104u )
		goto tr395;
	goto tr116;
case 331:
	if ( (*p) == 105u )
		goto tr396;
	goto tr116;
case 332:
	if ( (*p) == 101u )
		goto tr397;
	goto tr116;
case 333:
	if ( (*p) == 114u )
		goto tr398;
	goto tr116;
case 803:
	switch( (*p) ) {
		case 38u: goto tr1084;
		case 97u: goto tr1085;
		case 101u: goto tr1086;
		case 105u: goto tr1087;
		case 108u: goto tr1088;
		case 109u: goto tr1089;
		case 111u: goto tr1090;
		case 116u: goto tr1091;
		case 117u: goto tr1092;
		case 121u: goto tr1093;
	}
	goto tr1083;
case 334:
	switch( (*p) ) {
		case 111u: goto tr400;
		case 116u: goto tr401;
	}
	goto tr399;
case 335:
	if ( (*p) == 105u )
		goto tr402;
	goto tr399;
case 336:
	if ( (*p) == 110u )
		goto tr403;
	goto tr399;
case 337:
	if ( (*p) == 112u )
		goto tr404;
	goto tr399;
case 338:
	if ( (*p) == 99u )
		goto tr405;
	goto tr399;
case 339:
	if ( (*p) == 104u )
		goto tr406;
	goto tr399;
case 340:
	if ( (*p) == 97u )
		goto tr407;
	goto tr399;
case 341:
	switch( (*p) ) {
		case 109u: goto tr408;
		case 110u: goto tr409;
		case 115u: goto tr410;
	}
	goto tr399;
case 342:
	if ( (*p) == 98u )
		goto tr411;
	goto tr399;
case 343:
	if ( (*p) == 117u )
		goto tr412;
	goto tr399;
case 344:
	if ( (*p) == 101u )
		goto tr413;
	goto tr399;
case 345:
	if ( (*p) == 97u )
		goto tr414;
	goto tr399;
case 346:
	if ( (*p) == 114u )
		goto tr415;
	goto tr399;
case 347:
	if ( (*p) == 95u )
		goto tr416;
	goto tr399;
case 348:
	if ( (*p) == 66u )
		goto tr417;
	goto tr399;
case 349:
	if ( (*p) == 117u )
		goto tr418;
	goto tr399;
case 350:
	switch( (*p) ) {
		case 99u: goto tr419;
		case 100u: goto tr420;
	}
	goto tr399;
case 351:
	if ( (*p) == 105u )
		goto tr421;
	goto tr399;
case 352:
	if ( (*p) == 97u )
		goto tr422;
	goto tr399;
case 353:
	if ( (*p) == 110u )
		goto tr423;
	goto tr399;
case 354:
	if ( (*p) == 105u )
		goto tr424;
	goto tr399;
case 355:
	if ( (*p) == 97u )
		goto tr425;
	goto tr399;
case 356:
	if ( (*p) == 110u )
		goto tr426;
	goto tr399;
case 804:
	switch( (*p) ) {
		case 97u: goto tr1095;
		case 99u: goto tr1096;
		case 101u: goto tr1097;
		case 110u: goto tr1098;
		case 111u: goto tr1099;
		case 121u: goto tr1100;
	}
	goto tr1094;
case 357:
	switch( (*p) ) {
		case 108u: goto tr428;
		case 110u: goto tr429;
	}
	goto tr427;
case 358:
	if ( (*p) == 97u )
		goto tr430;
	goto tr427;
case 359:
	if ( (*p) == 121u )
		goto tr431;
	goto tr427;
case 360:
	if ( (*p) == 97u )
		goto tr432;
	goto tr427;
case 361:
	if ( (*p) == 108u )
		goto tr433;
	goto tr427;
case 362:
	if ( (*p) == 97u )
		goto tr434;
	goto tr427;
case 363:
	if ( (*p) == 109u )
		goto tr435;
	goto tr427;
case 364:
	if ( (*p) == 100u )
		goto tr436;
	goto tr427;
case 365:
	if ( (*p) == 97u )
		goto tr437;
	goto tr427;
case 366:
	if ( (*p) == 105u )
		goto tr438;
	goto tr427;
case 367:
	if ( (*p) == 99u )
		goto tr439;
	goto tr427;
case 805:
	if ( (*p) == 101u )
		goto tr1102;
	goto tr1101;
case 368:
	if ( (*p) == 116u )
		goto tr441;
	goto tr440;
case 369:
	if ( (*p) == 101u )
		goto tr442;
	goto tr440;
case 370:
	if ( (*p) == 105u )
		goto tr443;
	goto tr440;
case 371:
	if ( (*p) == 95u )
		goto tr444;
	goto tr440;
case 372:
	if ( (*p) == 77u )
		goto tr445;
	goto tr440;
case 373:
	if ( (*p) == 97u )
		goto tr446;
	goto tr440;
case 374:
	if ( (*p) == 121u )
		goto tr447;
	goto tr440;
case 375:
	if ( (*p) == 101u )
		goto tr448;
	goto tr440;
case 376:
	if ( (*p) == 107u )
		goto tr449;
	goto tr440;
case 377:
	if ( (*p) == 110u )
		goto tr450;
	goto tr427;
case 378:
	if ( (*p) == 103u )
		goto tr451;
	goto tr427;
case 379:
	if ( (*p) == 111u )
		goto tr452;
	goto tr427;
case 380:
	if ( (*p) == 108u )
		goto tr453;
	goto tr427;
case 381:
	if ( (*p) == 105u )
		goto tr454;
	goto tr427;
case 382:
	if ( (*p) == 97u )
		goto tr455;
	goto tr427;
case 383:
	if ( (*p) == 110u )
		goto tr456;
	goto tr427;
case 384:
	if ( (*p) == 97u )
		goto tr457;
	goto tr427;
case 385:
	if ( (*p) == 110u )
		goto tr458;
	goto tr427;
case 386:
	if ( (*p) == 109u )
		goto tr459;
	goto tr427;
case 387:
	if ( (*p) == 97u )
		goto tr460;
	goto tr427;
case 388:
	if ( (*p) == 114u )
		goto tr461;
	goto tr427;
case 806:
	switch( (*p) ) {
		case 100u: goto tr1104;
		case 101u: goto tr1105;
		case 107u: goto tr1106;
		case 108u: goto tr1107;
		case 111u: goto tr1108;
	}
	goto tr1103;
case 389:
	if ( (*p) == 119u )
		goto tr463;
	goto tr462;
case 390:
	if ( (*p) == 95u )
		goto tr464;
	goto tr462;
case 391:
	if ( (*p) == 84u )
		goto tr465;
	goto tr462;
case 392:
	if ( (*p) == 97u )
		goto tr466;
	goto tr462;
case 393:
	if ( (*p) == 105u )
		goto tr467;
	goto tr462;
case 394:
	if ( (*p) == 95u )
		goto tr468;
	goto tr462;
case 395:
	if ( (*p) == 76u )
		goto tr469;
	goto tr462;
case 396:
	if ( (*p) == 117u )
		goto tr470;
	goto tr462;
case 397:
	if ( (*p) == 101u )
		goto tr471;
	goto tr462;
case 398:
	if ( (*p) == 111u )
		goto tr472;
	goto tr462;
case 807:
	switch( (*p) ) {
		case 103u: goto tr1109;
		case 108u: goto tr1110;
		case 114u: goto tr1111;
		case 115u: goto tr1112;
	}
	goto tr1042;
case 399:
	if ( (*p) == 104u )
		goto tr473;
	goto tr116;
case 400:
	if ( (*p) == 97u )
		goto tr474;
	goto tr116;
case 401:
	if ( (*p) == 109u )
		goto tr475;
	goto tr116;
case 402:
	switch( (*p) ) {
		case 95u: goto tr476;
		case 100u: goto tr477;
	}
	goto tr116;
case 403:
	if ( (*p) == 67u )
		goto tr478;
	goto tr116;
case 404:
	if ( (*p) == 104u )
		goto tr479;
	goto tr116;
case 405:
	if ( (*p) == 105u )
		goto tr480;
	goto tr116;
case 406:
	if ( (*p) == 107u )
		goto tr481;
	goto tr116;
case 407:
	if ( (*p) == 105u )
		goto tr482;
	goto tr116;
case 408:
	if ( (*p) == 95u )
		goto tr483;
	goto tr116;
case 409:
	switch( (*p) ) {
		case 73u: goto tr484;
		case 80u: goto tr485;
		case 83u: goto tr486;
		case 84u: goto tr487;
	}
	goto tr116;
case 410:
	if ( (*p) == 116u )
		goto tr488;
	goto tr116;
case 411:
	if ( (*p) == 97u )
		goto tr489;
	goto tr116;
case 412:
	if ( (*p) == 108u )
		goto tr490;
	goto tr116;
case 413:
	if ( (*p) == 105u )
		goto tr491;
	goto tr116;
case 414:
	if ( (*p) == 99u )
		goto tr492;
	goto tr116;
case 415:
	if ( (*p) == 101u )
		goto tr493;
	goto tr116;
case 416:
	if ( (*p) == 114u )
		goto tr494;
	goto tr116;
case 417:
	if ( (*p) == 115u )
		goto tr495;
	goto tr116;
case 418:
	if ( (*p) == 105u )
		goto tr496;
	goto tr116;
case 419:
	if ( (*p) == 97u )
		goto tr497;
	goto tr116;
case 420:
	if ( (*p) == 110u )
		goto tr498;
	goto tr116;
case 421:
	if ( (*p) == 111u )
		goto tr499;
	goto tr116;
case 422:
	if ( (*p) == 117u )
		goto tr500;
	goto tr116;
case 423:
	if ( (*p) == 116u )
		goto tr501;
	goto tr116;
case 424:
	if ( (*p) == 104u )
		goto tr502;
	goto tr116;
case 425:
	if ( (*p) == 95u )
		goto tr503;
	goto tr116;
case 426:
	if ( (*p) == 65u )
		goto tr504;
	goto tr116;
case 427:
	if ( (*p) == 114u )
		goto tr505;
	goto tr116;
case 428:
	if ( (*p) == 97u )
		goto tr506;
	goto tr116;
case 429:
	if ( (*p) == 98u )
		goto tr507;
	goto tr116;
case 430:
	if ( (*p) == 105u )
		goto tr508;
	goto tr116;
case 431:
	if ( (*p) == 97u )
		goto tr509;
	goto tr116;
case 432:
	if ( (*p) == 110u )
		goto tr510;
	goto tr116;
case 433:
	if ( (*p) == 117u )
		goto tr511;
	goto tr116;
case 434:
	if ( (*p) == 114u )
		goto tr512;
	goto tr116;
case 435:
	if ( (*p) == 107u )
		goto tr513;
	goto tr116;
case 436:
	if ( (*p) == 105u )
		goto tr514;
	goto tr116;
case 437:
	if ( (*p) == 99u )
		goto tr515;
	goto tr116;
case 438:
	if ( (*p) == 105u )
		goto tr516;
	goto tr116;
case 439:
	if ( (*p) == 121u )
		goto tr517;
	goto tr116;
case 440:
	if ( (*p) == 97u )
		goto tr518;
	goto tr116;
case 441:
	if ( (*p) == 109u )
		goto tr519;
	goto tr116;
case 442:
	if ( (*p) == 97u )
		goto tr520;
	goto tr116;
case 443:
	if ( (*p) == 110u )
		goto tr521;
	goto tr116;
case 444:
	if ( (*p) == 121u )
		goto tr522;
	goto tr116;
case 445:
	if ( (*p) == 97u )
		goto tr523;
	goto tr116;
case 808:
	switch( (*p) ) {
		case 99u: goto tr1114;
		case 100u: goto tr1115;
		case 101u: goto tr1116;
		case 102u: goto tr1117;
		case 104u: goto tr1118;
		case 105u: goto tr1119;
		case 111u: goto tr1120;
		case 115u: goto tr1121;
	}
	goto tr1113;
case 446:
	switch( (*p) ) {
		case 97u: goto tr525;
		case 111u: goto tr526;
	}
	goto tr524;
case 447:
	if ( (*p) == 103u )
		goto tr527;
	goto tr524;
case 448:
	if ( (*p) == 115u )
		goto tr528;
	goto tr524;
case 449:
	if ( (*p) == 95u )
		goto tr529;
	goto tr524;
case 450:
	if ( (*p) == 80u )
		goto tr530;
	goto tr524;
case 451:
	if ( (*p) == 97u )
		goto tr531;
	goto tr524;
case 452:
	if ( (*p) == 101u )
		goto tr532;
	goto tr524;
case 453:
	if ( (*p) == 110u )
		goto tr533;
	goto tr524;
case 454:
	if ( (*p) == 105u )
		goto tr534;
	goto tr524;
case 455:
	if ( (*p) == 99u )
		goto tr535;
	goto tr524;
case 456:
	if ( (*p) == 105u )
		goto tr536;
	goto tr524;
case 457:
	if ( (*p) == 97u )
		goto tr537;
	goto tr524;
case 458:
	if ( (*p) == 110u )
		goto tr538;
	goto tr524;
case 809:
	switch( (*p) ) {
		case 101u: goto tr1122;
		case 117u: goto tr1123;
	}
	goto tr1042;
case 459:
	if ( (*p) == 106u )
		goto tr539;
	goto tr116;
case 460:
	if ( (*p) == 97u )
		goto tr540;
	goto tr116;
case 461:
	if ( (*p) == 110u )
		goto tr541;
	goto tr116;
case 462:
	if ( (*p) == 103u )
		goto tr542;
	goto tr116;
case 463:
	if ( (*p) == 110u )
		goto tr543;
	goto tr116;
case 464:
	if ( (*p) == 105u )
		goto tr544;
	goto tr116;
case 465:
	if ( (*p) == 99u )
		goto tr545;
	goto tr116;
case 810:
	switch( (*p) ) {
		case 97u: goto tr1125;
		case 99u: goto tr1126;
		case 104u: goto tr1127;
		case 105u: goto tr1128;
		case 107u: goto tr1129;
		case 109u: goto tr1130;
		case 111u: goto tr1131;
		case 117u: goto tr1132;
		case 121u: goto tr1133;
	}
	goto tr1124;
case 466:
	switch( (*p) ) {
		case 109u: goto tr547;
		case 117u: goto tr548;
	}
	goto tr546;
case 467:
	if ( (*p) == 97u )
		goto tr549;
	goto tr546;
case 468:
	if ( (*p) == 114u )
		goto tr550;
	goto tr546;
case 469:
	if ( (*p) == 105u )
		goto tr551;
	goto tr546;
case 470:
	if ( (*p) == 116u )
		goto tr552;
	goto tr546;
case 471:
	if ( (*p) == 97u )
		goto tr553;
	goto tr546;
case 472:
	if ( (*p) == 110u )
		goto tr554;
	goto tr546;
case 473:
	if ( (*p) == 114u )
		goto tr555;
	goto tr546;
case 474:
	if ( (*p) == 97u )
		goto tr556;
	goto tr546;
case 475:
	if ( (*p) == 115u )
		goto tr557;
	goto tr546;
case 476:
	if ( (*p) == 104u )
		goto tr558;
	goto tr546;
case 477:
	if ( (*p) == 116u )
		goto tr559;
	goto tr546;
case 478:
	if ( (*p) == 114u )
		goto tr560;
	goto tr546;
case 479:
	if ( (*p) == 97u )
		goto tr561;
	goto tr546;
case 480:
	if ( (*p) == 97u )
		goto tr562;
	goto tr546;
case 481:
	if ( (*p) == 118u )
		goto tr563;
	goto tr546;
case 482:
	if ( (*p) == 105u )
		goto tr564;
	goto tr546;
case 483:
	if ( (*p) == 97u )
		goto tr565;
	goto tr546;
case 484:
	if ( (*p) == 110u )
		goto tr566;
	goto tr546;
case 485:
	if ( (*p) == 110u )
		goto tr567;
	goto tr546;
case 486:
	if ( (*p) == 104u )
		goto tr568;
	goto tr546;
case 487:
	if ( (*p) == 97u )
		goto tr569;
	goto tr546;
case 488:
	if ( (*p) == 108u )
		goto tr570;
	goto tr546;
case 489:
	if ( (*p) == 97u )
		goto tr571;
	goto tr546;
case 490:
	if ( (*p) == 110u )
		goto tr572;
	goto tr546;
case 491:
	if ( (*p) == 100u )
		goto tr573;
	goto tr546;
case 492:
	if ( (*p) == 97u )
		goto tr574;
	goto tr546;
case 493:
	if ( (*p) == 110u )
		goto tr575;
	goto tr546;
case 494:
	if ( (*p) == 101u )
		goto tr576;
	goto tr546;
case 495:
	if ( (*p) == 115u )
		goto tr577;
	goto tr546;
case 496:
	if ( (*p) == 101u )
		goto tr578;
	goto tr546;
case 497:
	switch( (*p) ) {
		case 108u: goto tr579;
		case 114u: goto tr580;
	}
	goto tr546;
case 498:
	if ( (*p) == 111u )
		goto tr581;
	goto tr546;
case 499:
	if ( (*p) == 116u )
		goto tr582;
	goto tr546;
case 500:
	if ( (*p) == 105u )
		goto tr583;
	goto tr546;
case 501:
	if ( (*p) == 95u )
		goto tr584;
	goto tr546;
case 502:
	if ( (*p) == 78u )
		goto tr585;
	goto tr546;
case 503:
	if ( (*p) == 97u )
		goto tr586;
	goto tr546;
case 504:
	if ( (*p) == 103u )
		goto tr587;
	goto tr546;
case 505:
	if ( (*p) == 114u )
		goto tr588;
	goto tr546;
case 506:
	if ( (*p) == 105u )
		goto tr589;
	goto tr546;
case 507:
	if ( (*p) == 105u )
		goto tr590;
	goto tr546;
case 508:
	if ( (*p) == 97u )
		goto tr591;
	goto tr546;
case 509:
	if ( (*p) == 99u )
		goto tr592;
	goto tr546;
case 811:
	switch( (*p) ) {
		case 97u: goto tr1134;
		case 101u: goto tr1135;
		case 104u: goto tr1136;
		case 105u: goto tr1137;
	}
	goto tr1042;
case 510:
	switch( (*p) ) {
		case 103u: goto tr593;
		case 105u: goto tr594;
		case 109u: goto tr595;
	}
	goto tr116;
case 511:
	switch( (*p) ) {
		case 97u: goto tr596;
		case 98u: goto tr597;
	}
	goto tr116;
case 512:
	if ( (*p) == 108u )
		goto tr598;
	goto tr116;
case 513:
	if ( (*p) == 111u )
		goto tr599;
	goto tr116;
case 514:
	if ( (*p) == 103u )
		goto tr600;
	goto tr116;
case 515:
	if ( (*p) == 97u )
		goto tr601;
	goto tr116;
case 516:
	if ( (*p) == 110u )
		goto tr602;
	goto tr116;
case 517:
	if ( (*p) == 119u )
		goto tr603;
	goto tr116;
case 518:
	if ( (*p) == 97u )
		goto tr604;
	goto tr116;
case 519:
	if ( (*p) == 95u )
		goto tr605;
	goto tr116;
case 520:
	switch( (*p) ) {
		case 76u: goto tr606;
		case 84u: goto tr607;
		case 86u: goto tr608;
	}
	goto tr116;
case 521:
	if ( (*p) == 101u )
		goto tr609;
	goto tr116;
case 522:
	if ( (*p) == 104u )
		goto tr610;
	goto tr116;
case 523:
	if ( (*p) == 97u )
		goto tr611;
	goto tr116;
case 524:
	if ( (*p) == 109u )
		goto tr612;
	goto tr116;
case 525:
	if ( (*p) == 105u )
		goto tr613;
	goto tr116;
case 526:
	if ( (*p) == 101u )
		goto tr614;
	goto tr116;
case 527:
	if ( (*p) == 116u )
		goto tr615;
	goto tr116;
case 528:
	if ( (*p) == 105u )
		goto tr616;
	goto tr116;
case 529:
	if ( (*p) == 108u )
		goto tr617;
	goto tr116;
case 530:
	if ( (*p) == 108u )
		goto tr618;
	goto tr116;
case 531:
	if ( (*p) == 117u )
		goto tr619;
	goto tr116;
case 532:
	if ( (*p) == 103u )
		goto tr620;
	goto tr116;
case 533:
	if ( (*p) == 117u )
		goto tr621;
	goto tr116;
case 534:
	if ( (*p) == 97u )
		goto tr622;
	goto tr116;
case 535:
	switch( (*p) ) {
		case 97u: goto tr623;
		case 105u: goto tr624;
	}
	goto tr116;
case 536:
	if ( (*p) == 110u )
		goto tr625;
	goto tr116;
case 537:
	if ( (*p) == 97u )
		goto tr626;
	goto tr116;
case 538:
	switch( (*p) ) {
		case 98u: goto tr627;
		case 102u: goto tr628;
	}
	goto tr116;
case 539:
	if ( (*p) == 101u )
		goto tr629;
	goto tr116;
case 540:
	if ( (*p) == 116u )
		goto tr630;
	goto tr116;
case 541:
	if ( (*p) == 97u )
		goto tr631;
	goto tr116;
case 542:
	if ( (*p) == 110u )
		goto tr632;
	goto tr116;
case 543:
	if ( (*p) == 105u )
		goto tr633;
	goto tr116;
case 544:
	if ( (*p) == 110u )
		goto tr634;
	goto tr116;
case 545:
	if ( (*p) == 97u )
		goto tr635;
	goto tr116;
case 546:
	if ( (*p) == 103u )
		goto tr636;
	goto tr116;
case 547:
	if ( (*p) == 104u )
		goto tr637;
	goto tr116;
case 812:
	if ( (*p) == 103u )
		goto tr1138;
	goto tr1042;
case 548:
	if ( (*p) == 97u )
		goto tr638;
	goto tr116;
case 549:
	if ( (*p) == 114u )
		goto tr639;
	goto tr116;
case 550:
	if ( (*p) == 105u )
		goto tr640;
	goto tr116;
case 551:
	if ( (*p) == 116u )
		goto tr641;
	goto tr116;
case 552:
	if ( (*p) == 105u )
		goto tr642;
	goto tr116;
case 553:
	if ( (*p) == 99u )
		goto tr643;
	goto tr116;
case 813:
	if ( (*p) == 97u )
		goto tr1139;
	goto tr1042;
case 554:
	if ( (*p) == 105u )
		goto tr644;
	goto tr116;
case 814:
	switch( (*p) ) {
		case 97u: goto tr1140;
		case 112u: goto tr1141;
		case 115u: goto tr1142;
		case 119u: goto tr1143;
	}
	goto tr1042;
case 555:
	if ( (*p) == 110u )
		goto tr645;
	goto tr116;
case 556:
	if ( (*p) == 115u )
		goto tr646;
	goto tr116;
case 557:
	if ( (*p) == 112u )
		goto tr647;
	goto tr116;
case 558:
	if ( (*p) == 100u )
		goto tr648;
	goto tr116;
case 815:
	if ( (*p) == 105u )
		goto tr1144;
	goto tr1042;
case 816:
	switch( (*p) ) {
		case 108u: goto tr1146;
		case 112u: goto tr1147;
		case 115u: goto tr1148;
	}
	goto tr1145;
case 818:
	switch( (*p) ) {
		case 67u: goto tr1150;
		case 76u: goto tr1151;
		case 77u: goto tr1152;
		case 78u: goto tr1153;
		case 80u: goto tr1154;
		case 83u: goto tr1155;
		case 90u: goto tr1156;
	}
	goto tr1149;
case 819:
	_widec = (*p);
	if ( (*p) < 224u ) {
		if ( (*p) > 191u ) {
			if ( 192u <= (*p) && (*p) <= 223u ) {
				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 128u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else if ( (*p) > 239u ) {
		if ( (*p) > 247u ) {
			if ( 248u <= (*p) )
 {				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 240u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	switch( _widec ) {
		case 45: goto tr1158;
		case 91: goto tr1159;
		case 92: goto tr1160;
		case 93: goto tr1161;
	}
	if ( _widec < 704 ) {
		if ( _widec < 384 ) {
			if ( _widec <= 127 )
				goto tr1157;
		} else if ( _widec > 511 ) {
			if ( 640 <= _widec && _widec <= 703 )
				goto tr1162;
		} else
			goto tr1157;
	} else if ( _widec > 735 ) {
		if ( _widec < 752 ) {
			if ( 736 <= _widec && _widec <= 751 )
				goto tr1164;
		} else if ( _widec > 759 ) {
			if ( 760 <= _widec && _widec <= 767 )
				goto tr1162;
		} else
			goto tr1165;
	} else
		goto tr1163;
	goto tr877;
case 820:
	switch( (*p) ) {
		case 46u: goto tr655;
		case 58u: goto tr1167;
		case 61u: goto tr852;
	}
	goto tr1166;
case 563:
	switch( (*p) ) {
		case 46u: goto tr656;
		case 92u: goto tr657;
		case 93u: goto tr654;
	}
	goto tr655;
case 564:
	switch( (*p) ) {
		case 46u: goto tr656;
		case 92u: goto tr657;
		case 93u: goto tr658;
	}
	goto tr655;
case 565:
	switch( (*p) ) {
		case 46u: goto tr656;
		case 92u: goto tr657;
	}
	goto tr655;
case 566:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 94u: goto tr662;
		case 97u: goto tr663;
		case 98u: goto tr664;
		case 99u: goto tr665;
		case 100u: goto tr666;
		case 103u: goto tr667;
		case 108u: goto tr668;
		case 112u: goto tr669;
		case 115u: goto tr670;
		case 117u: goto tr671;
		case 119u: goto tr672;
		case 120u: goto tr673;
	}
	goto tr659;
case 567:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 568:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr674;
	}
	goto tr659;
case 569:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
	}
	goto tr659;
case 570:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr675;
		case 98u: goto tr676;
		case 99u: goto tr677;
		case 100u: goto tr678;
		case 103u: goto tr679;
		case 108u: goto tr680;
		case 112u: goto tr681;
		case 115u: goto tr682;
		case 117u: goto tr683;
		case 119u: goto tr684;
		case 120u: goto tr685;
	}
	goto tr659;
case 571:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr686;
		case 115u: goto tr687;
	}
	goto tr659;
case 572:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr688;
		case 112u: goto tr689;
	}
	goto tr659;
case 573:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 117u: goto tr690;
	}
	goto tr659;
case 574:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 109u: goto tr691;
	}
	goto tr659;
case 575:
	switch( (*p) ) {
		case 58u: goto tr692;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 576:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr693;
	}
	goto tr659;
case 577:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 104u: goto tr694;
	}
	goto tr659;
case 578:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr695;
	}
	goto tr659;
case 579:
	switch( (*p) ) {
		case 58u: goto tr696;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 580:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr697;
	}
	goto tr659;
case 581:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr698;
	}
	goto tr659;
case 582:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr699;
	}
	goto tr659;
case 583:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr700;
	}
	goto tr659;
case 584:
	switch( (*p) ) {
		case 58u: goto tr701;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 585:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr702;
	}
	goto tr659;
case 586:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr703;
	}
	goto tr659;
case 587:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr704;
	}
	goto tr659;
case 588:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr705;
	}
	goto tr659;
case 589:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 107u: goto tr706;
	}
	goto tr659;
case 590:
	switch( (*p) ) {
		case 58u: goto tr707;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 591:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr708;
	}
	goto tr659;
case 592:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr709;
	}
	goto tr659;
case 593:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr710;
	}
	goto tr659;
case 594:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr711;
	}
	goto tr659;
case 595:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr712;
	}
	goto tr659;
case 596:
	switch( (*p) ) {
		case 58u: goto tr713;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 597:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr714;
	}
	goto tr659;
case 598:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr715;
	}
	goto tr659;
case 599:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 103u: goto tr716;
	}
	goto tr659;
case 600:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr717;
	}
	goto tr659;
case 601:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr718;
	}
	goto tr659;
case 602:
	switch( (*p) ) {
		case 58u: goto tr719;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 603:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr720;
	}
	goto tr659;
case 604:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr721;
	}
	goto tr659;
case 605:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr722;
	}
	goto tr659;
case 606:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr723;
	}
	goto tr659;
case 607:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 104u: goto tr724;
	}
	goto tr659;
case 608:
	switch( (*p) ) {
		case 58u: goto tr725;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 609:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr726;
	}
	goto tr659;
case 610:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 111u: goto tr727;
	}
	goto tr659;
case 611:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 119u: goto tr728;
	}
	goto tr659;
case 612:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr729;
	}
	goto tr659;
case 613:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr730;
	}
	goto tr659;
case 614:
	switch( (*p) ) {
		case 58u: goto tr731;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 615:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr732;
	}
	goto tr659;
case 616:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr733;
		case 117u: goto tr734;
	}
	goto tr659;
case 617:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr735;
	}
	goto tr659;
case 618:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr736;
	}
	goto tr659;
case 619:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr737;
	}
	goto tr659;
case 620:
	switch( (*p) ) {
		case 58u: goto tr738;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 621:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr739;
	}
	goto tr659;
case 622:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr740;
	}
	goto tr659;
case 623:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr741;
	}
	goto tr659;
case 624:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr742;
	}
	goto tr659;
case 625:
	switch( (*p) ) {
		case 58u: goto tr743;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 626:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr744;
	}
	goto tr659;
case 627:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr745;
	}
	goto tr659;
case 628:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr746;
	}
	goto tr659;
case 629:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr747;
	}
	goto tr659;
case 630:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr748;
	}
	goto tr659;
case 631:
	switch( (*p) ) {
		case 58u: goto tr749;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 632:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr750;
	}
	goto tr659;
case 633:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr751;
	}
	goto tr659;
case 634:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr752;
	}
	goto tr659;
case 635:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr753;
	}
	goto tr659;
case 636:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr754;
	}
	goto tr659;
case 637:
	switch( (*p) ) {
		case 58u: goto tr755;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 638:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr756;
	}
	goto tr659;
case 639:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 111u: goto tr757;
	}
	goto tr659;
case 640:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr758;
	}
	goto tr659;
case 641:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 100u: goto tr759;
	}
	goto tr659;
case 642:
	switch( (*p) ) {
		case 58u: goto tr760;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 643:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr761;
	}
	goto tr659;
case 644:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 100u: goto tr762;
	}
	goto tr659;
case 645:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr763;
	}
	goto tr659;
case 646:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 103u: goto tr764;
	}
	goto tr659;
case 647:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr765;
	}
	goto tr659;
case 648:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr766;
	}
	goto tr659;
case 649:
	switch( (*p) ) {
		case 58u: goto tr767;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 650:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr768;
	}
	goto tr659;
case 651:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr769;
		case 115u: goto tr770;
	}
	goto tr659;
case 652:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr771;
		case 112u: goto tr772;
	}
	goto tr659;
case 653:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 117u: goto tr773;
	}
	goto tr659;
case 654:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 109u: goto tr774;
	}
	goto tr659;
case 655:
	switch( (*p) ) {
		case 58u: goto tr775;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 656:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr776;
	}
	goto tr659;
case 657:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 104u: goto tr777;
	}
	goto tr659;
case 658:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr778;
	}
	goto tr659;
case 659:
	switch( (*p) ) {
		case 58u: goto tr779;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 660:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr780;
	}
	goto tr659;
case 661:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr781;
	}
	goto tr659;
case 662:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr782;
	}
	goto tr659;
case 663:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr783;
	}
	goto tr659;
case 664:
	switch( (*p) ) {
		case 58u: goto tr784;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 665:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr785;
	}
	goto tr659;
case 666:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr786;
	}
	goto tr659;
case 667:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr787;
	}
	goto tr659;
case 668:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr788;
	}
	goto tr659;
case 669:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 107u: goto tr789;
	}
	goto tr659;
case 670:
	switch( (*p) ) {
		case 58u: goto tr790;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 671:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr791;
	}
	goto tr659;
case 672:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr792;
	}
	goto tr659;
case 673:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr793;
	}
	goto tr659;
case 674:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr794;
	}
	goto tr659;
case 675:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 108u: goto tr795;
	}
	goto tr659;
case 676:
	switch( (*p) ) {
		case 58u: goto tr796;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 677:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr797;
	}
	goto tr659;
case 678:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr798;
	}
	goto tr659;
case 679:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 103u: goto tr799;
	}
	goto tr659;
case 680:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr800;
	}
	goto tr659;
case 681:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr801;
	}
	goto tr659;
case 682:
	switch( (*p) ) {
		case 58u: goto tr802;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 683:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr803;
	}
	goto tr659;
case 684:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr804;
	}
	goto tr659;
case 685:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr805;
	}
	goto tr659;
case 686:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr806;
	}
	goto tr659;
case 687:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 104u: goto tr807;
	}
	goto tr659;
case 688:
	switch( (*p) ) {
		case 58u: goto tr808;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 689:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr809;
	}
	goto tr659;
case 690:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 111u: goto tr810;
	}
	goto tr659;
case 691:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 119u: goto tr811;
	}
	goto tr659;
case 692:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr812;
	}
	goto tr659;
case 693:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr813;
	}
	goto tr659;
case 694:
	switch( (*p) ) {
		case 58u: goto tr814;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 695:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr815;
	}
	goto tr659;
case 696:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr816;
		case 117u: goto tr817;
	}
	goto tr659;
case 697:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr818;
	}
	goto tr659;
case 698:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr819;
	}
	goto tr659;
case 699:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr820;
	}
	goto tr659;
case 700:
	switch( (*p) ) {
		case 58u: goto tr821;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 701:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr822;
	}
	goto tr659;
case 702:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 110u: goto tr823;
	}
	goto tr659;
case 703:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr824;
	}
	goto tr659;
case 704:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr825;
	}
	goto tr659;
case 705:
	switch( (*p) ) {
		case 58u: goto tr826;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 706:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr827;
	}
	goto tr659;
case 707:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr828;
	}
	goto tr659;
case 708:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 97u: goto tr829;
	}
	goto tr659;
case 709:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 99u: goto tr830;
	}
	goto tr659;
case 710:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr831;
	}
	goto tr659;
case 711:
	switch( (*p) ) {
		case 58u: goto tr832;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 712:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr833;
	}
	goto tr659;
case 713:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr834;
	}
	goto tr659;
case 714:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 112u: goto tr835;
	}
	goto tr659;
case 715:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 101u: goto tr836;
	}
	goto tr659;
case 716:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr837;
	}
	goto tr659;
case 717:
	switch( (*p) ) {
		case 58u: goto tr838;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 718:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr839;
	}
	goto tr659;
case 719:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 111u: goto tr840;
	}
	goto tr659;
case 720:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 114u: goto tr841;
	}
	goto tr659;
case 721:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 100u: goto tr842;
	}
	goto tr659;
case 722:
	switch( (*p) ) {
		case 58u: goto tr843;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 723:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr844;
	}
	goto tr659;
case 724:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 100u: goto tr845;
	}
	goto tr659;
case 725:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr846;
	}
	goto tr659;
case 726:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 103u: goto tr847;
	}
	goto tr659;
case 727:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 105u: goto tr848;
	}
	goto tr659;
case 728:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr654;
		case 116u: goto tr849;
	}
	goto tr659;
case 729:
	switch( (*p) ) {
		case 58u: goto tr850;
		case 92u: goto tr661;
		case 93u: goto tr654;
	}
	goto tr659;
case 730:
	switch( (*p) ) {
		case 58u: goto tr660;
		case 92u: goto tr661;
		case 93u: goto tr851;
	}
	goto tr659;
case 731:
	switch( (*p) ) {
		case 61u: goto tr853;
		case 92u: goto tr854;
		case 93u: goto tr654;
	}
	goto tr852;
case 732:
	switch( (*p) ) {
		case 61u: goto tr853;
		case 92u: goto tr854;
		case 93u: goto tr658;
	}
	goto tr852;
case 733:
	switch( (*p) ) {
		case 61u: goto tr853;
		case 92u: goto tr854;
	}
	goto tr852;
case 821:
	switch( (*p) ) {
		case 48u: goto tr1169;
		case 68u: goto tr1173;
		case 69u: goto tr1174;
		case 72u: goto tr1175;
		case 76u: goto tr1176;
		case 78u: goto tr1176;
		case 80u: goto tr1177;
		case 81u: goto tr1178;
		case 83u: goto tr1179;
		case 85u: goto tr1176;
		case 86u: goto tr1180;
		case 87u: goto tr1181;
		case 97u: goto tr1182;
		case 98u: goto tr1183;
		case 99u: goto tr1184;
		case 100u: goto tr1185;
		case 101u: goto tr1186;
		case 102u: goto tr1187;
		case 103u: goto tr1188;
		case 104u: goto tr1189;
		case 108u: goto tr1176;
		case 110u: goto tr1190;
		case 111u: goto tr1191;
		case 112u: goto tr1192;
		case 114u: goto tr1193;
		case 115u: goto tr1194;
		case 116u: goto tr1195;
		case 117u: goto tr1176;
		case 118u: goto tr1196;
		case 119u: goto tr1197;
		case 120u: goto tr1198;
	}
	if ( (*p) < 56u ) {
		if ( 49u <= (*p) && (*p) <= 55u )
			goto tr1170;
	} else if ( (*p) > 57u ) {
		if ( (*p) > 90u ) {
			if ( 105u <= (*p) && (*p) <= 122u )
				goto tr1172;
		} else if ( (*p) >= 65u )
			goto tr1172;
	} else
		goto tr1171;
	goto tr1168;
case 822:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr1200;
	goto tr1199;
case 823:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr1201;
	goto tr1199;
case 824:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr1203;
	goto tr1202;
case 825:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr1204;
	goto tr1202;
case 826:
	if ( (*p) == 123u )
		goto tr1207;
	goto tr1206;
case 827:
	goto tr1209;
case 828:
	if ( (*p) == 123u )
		goto tr1211;
	goto tr1210;
case 734:
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr856;
	goto tr855;
case 735:
	if ( (*p) == 125u )
		goto tr857;
	if ( 48u <= (*p) && (*p) <= 55u )
		goto tr856;
	goto tr855;
case 829:
	if ( (*p) == 123u )
		goto tr1214;
	goto tr1213;
case 830:
	if ( (*p) == 123u )
		goto tr1219;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr1216;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr1218;
	} else
		goto tr1217;
	goto tr1215;
case 831:
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr1220;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr1222;
	} else
		goto tr1221;
	goto tr1215;
case 832:
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr859;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr859;
	} else
		goto tr859;
	goto tr1223;
case 736:
	if ( (*p) == 125u )
		goto tr860;
	if ( (*p) < 65u ) {
		if ( 48u <= (*p) && (*p) <= 57u )
			goto tr859;
	} else if ( (*p) > 70u ) {
		if ( 97u <= (*p) && (*p) <= 102u )
			goto tr859;
	} else
		goto tr859;
	goto tr858;
case 833:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1225;
	goto tr1224;
case 834:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1226;
	goto tr1224;
case 737:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr862;
	goto tr861;
case 835:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1227;
	goto tr1224;
case 738:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr863;
	goto tr861;
case 739:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr864;
	goto tr861;
case 836:
	_widec = (*p);
	if ( (*p) > 93u ) {
		if ( 94u <= (*p) && (*p) <= 94u ) {
			_widec = (short)(768u + ((*p) - 0u));
			if ( 
#line 478 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 inCharClassEarly  ) _widec += 256;
		}
	} else if ( (*p) >= 93u ) {
		_widec = (short)(768u + ((*p) - 0u));
		if ( 
#line 478 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 inCharClassEarly  ) _widec += 256;
	}
	switch( _widec ) {
		case 92: goto tr1229;
		case 1117: goto tr1230;
		case 1118: goto tr1231;
	}
	if ( _widec < 95 ) {
		if ( _widec <= 91 )
			goto tr1228;
	} else if ( _widec > 255 ) {
		if ( 861 <= _widec && _widec <= 862 )
			goto tr1228;
	} else
		goto tr1228;
	goto tr877;
case 837:
	switch( (*p) ) {
		case 69u: goto tr1233;
		case 81u: goto tr1234;
	}
	goto tr1232;
case 838:
	_widec = (*p);
	if ( (*p) < 224u ) {
		if ( (*p) > 191u ) {
			if ( 192u <= (*p) && (*p) <= 223u ) {
				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 128u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else if ( (*p) > 239u ) {
		if ( (*p) > 247u ) {
			if ( 248u <= (*p) )
 {				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 240u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( _widec == 92 )
		goto tr1236;
	if ( _widec < 704 ) {
		if ( _widec < 384 ) {
			if ( _widec <= 127 )
				goto tr1235;
		} else if ( _widec > 511 ) {
			if ( 640 <= _widec && _widec <= 703 )
				goto tr1237;
		} else
			goto tr1235;
	} else if ( _widec > 735 ) {
		if ( _widec < 752 ) {
			if ( 736 <= _widec && _widec <= 751 )
				goto tr1239;
		} else if ( _widec > 759 ) {
			if ( 760 <= _widec && _widec <= 767 )
				goto tr1237;
		} else
			goto tr1240;
	} else
		goto tr1238;
	goto tr877;
case 839:
	if ( (*p) == 69u )
		goto tr1242;
	goto tr1241;
case 840:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1244;
	goto tr1243;
case 841:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1245;
	goto tr1243;
case 740:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr866;
	goto tr865;
case 842:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1246;
	goto tr1243;
case 741:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr867;
	goto tr865;
case 742:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr868;
	goto tr865;
case 843:
	_widec = (*p);
	if ( (*p) < 224u ) {
		if ( (*p) > 191u ) {
			if ( 192u <= (*p) && (*p) <= 223u ) {
				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 128u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else if ( (*p) > 239u ) {
		if ( (*p) > 247u ) {
			if ( 248u <= (*p) )
 {				_widec = (short)(256u + ((*p) - 0u));
				if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
			}
		} else if ( (*p) >= 240u ) {
			_widec = (short)(256u + ((*p) - 0u));
			if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
		}
	} else {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( _widec == 92 )
		goto tr1248;
	if ( _widec < 704 ) {
		if ( _widec < 384 ) {
			if ( _widec <= 127 )
				goto tr1247;
		} else if ( _widec > 511 ) {
			if ( 640 <= _widec && _widec <= 703 )
				goto tr1249;
		} else
			goto tr1247;
	} else if ( _widec > 735 ) {
		if ( _widec < 752 ) {
			if ( 736 <= _widec && _widec <= 751 )
				goto tr1251;
		} else if ( _widec > 759 ) {
			if ( 760 <= _widec && _widec <= 767 )
				goto tr1249;
		} else
			goto tr1252;
	} else
		goto tr1250;
	goto tr877;
case 844:
	if ( (*p) == 69u )
		goto tr1254;
	goto tr1253;
case 845:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1256;
	goto tr1255;
case 846:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1257;
	goto tr1255;
case 743:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr870;
	goto tr869;
case 847:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr1258;
	goto tr1255;
case 744:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr871;
	goto tr869;
case 745:
	_widec = (*p);
	if ( 128u <= (*p) && (*p) <= 191u ) {
		_widec = (short)(256u + ((*p) - 0u));
		if ( 
#line 476 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
 mode.utf8  ) _widec += 256;
	}
	if ( 640 <= _widec && _widec <= 703 )
		goto tr872;
	goto tr869;
case 848:
	if ( (*p) == 41u )
		goto tr1260;
	goto tr1259;
case 849:
	if ( (*p) == 10u )
		goto tr1262;
	goto tr1261;
	}

	tr877: cs = 0; goto _again;
	tr649: cs = 0; goto f174;
	tr895: cs = 1; goto _again;
	tr896: cs = 2; goto _again;
	tr23: cs = 3; goto _again;
	tr25: cs = 4; goto f17;
	tr899: cs = 5; goto f237;
	tr27: cs = 6; goto f17;
	tr28: cs = 7; goto _again;
	tr30: cs = 8; goto f17;
	tr32: cs = 9; goto f21;
	tr900: cs = 9; goto f238;
	tr901: cs = 10; goto f238;
	tr902: cs = 11; goto f237;
	tr33: cs = 12; goto f17;
	tr903: cs = 13; goto _again;
	tr35: cs = 14; goto _again;
	tr904: cs = 15; goto f17;
	tr39: cs = 16; goto _again;
	tr42: cs = 17; goto _again;
	tr41: cs = 18; goto f17;
	tr44: cs = 19; goto _again;
	tr47: cs = 20; goto f30;
	tr48: cs = 21; goto f17;
	tr50: cs = 22; goto _again;
	tr908: cs = 23; goto f237;
	tr909: cs = 24; goto f237;
	tr51: cs = 25; goto f17;
	tr910: cs = 26; goto _again;
	tr53: cs = 27; goto _again;
	tr54: cs = 28; goto f33;
	tr56: cs = 29; goto _again;
	tr57: cs = 30; goto _again;
	tr58: cs = 31; goto _again;
	tr60: cs = 32; goto _again;
	tr61: cs = 33; goto _again;
	tr62: cs = 34; goto _again;
	tr64: cs = 35; goto _again;
	tr65: cs = 36; goto _again;
	tr66: cs = 37; goto _again;
	tr68: cs = 38; goto _again;
	tr977: cs = 39; goto f40;
	tr71: cs = 40; goto _again;
	tr979: cs = 41; goto f302;
	tr72: cs = 42; goto f40;
	tr75: cs = 43; goto f39;
	tr76: cs = 44; goto f39;
	tr78: cs = 45; goto f39;
	tr73: cs = 46; goto f21;
	tr79: cs = 47; goto f21;
	tr81: cs = 48; goto f21;
	tr74: cs = 49; goto f17;
	tr987: cs = 50; goto f237;
	tr84: cs = 51; goto f17;
	tr988: cs = 52; goto f237;
	tr86: cs = 53; goto f17;
	tr989: cs = 54; goto f237;
	tr88: cs = 55; goto f17;
	tr991: cs = 56; goto _again;
	tr91: cs = 57; goto _again;
	tr94: cs = 58; goto _again;
	tr98: cs = 59; goto f53;
	tr97: cs = 60; goto _again;
	tr100: cs = 61; goto f55;
	tr1010: cs = 62; goto _again;
	tr1011: cs = 63; goto _again;
	tr105: cs = 64; goto _again;
	tr108: cs = 65; goto _again;
	tr1016: cs = 66; goto _again;
	tr110: cs = 67; goto _again;
	tr1017: cs = 68; goto _again;
	tr112: cs = 69; goto _again;
	tr114: cs = 70; goto _again;
	tr1043: cs = 71; goto _again;
	tr1044: cs = 72; goto _again;
	tr118: cs = 73; goto _again;
	tr120: cs = 74; goto _again;
	tr121: cs = 75; goto _again;
	tr119: cs = 76; goto _again;
	tr123: cs = 77; goto _again;
	tr124: cs = 78; goto _again;
	tr125: cs = 79; goto _again;
	tr126: cs = 80; goto _again;
	tr1045: cs = 81; goto _again;
	tr128: cs = 82; goto _again;
	tr129: cs = 83; goto _again;
	tr130: cs = 84; goto _again;
	tr131: cs = 85; goto _again;
	tr1046: cs = 86; goto _again;
	tr133: cs = 87; goto _again;
	tr136: cs = 88; goto _again;
	tr137: cs = 89; goto _again;
	tr138: cs = 90; goto _again;
	tr139: cs = 91; goto _again;
	tr134: cs = 92; goto _again;
	tr141: cs = 93; goto _again;
	tr135: cs = 94; goto _again;
	tr143: cs = 95; goto _again;
	tr1047: cs = 96; goto _again;
	tr145: cs = 97; goto _again;
	tr146: cs = 98; goto _again;
	tr147: cs = 99; goto _again;
	tr148: cs = 100; goto _again;
	tr1048: cs = 101; goto _again;
	tr150: cs = 102; goto _again;
	tr151: cs = 103; goto _again;
	tr152: cs = 104; goto _again;
	tr153: cs = 105; goto _again;
	tr154: cs = 106; goto _again;
	tr1049: cs = 107; goto _again;
	tr156: cs = 108; goto _again;
	tr157: cs = 109; goto _again;
	tr159: cs = 110; goto _again;
	tr158: cs = 111; goto _again;
	tr161: cs = 112; goto _again;
	tr162: cs = 113; goto _again;
	tr1050: cs = 114; goto _again;
	tr164: cs = 115; goto _again;
	tr166: cs = 116; goto _again;
	tr167: cs = 117; goto _again;
	tr168: cs = 118; goto _again;
	tr169: cs = 119; goto _again;
	tr165: cs = 120; goto _again;
	tr171: cs = 121; goto _again;
	tr1052: cs = 122; goto _again;
	tr174: cs = 123; goto _again;
	tr176: cs = 124; goto _again;
	tr177: cs = 125; goto _again;
	tr178: cs = 126; goto _again;
	tr179: cs = 127; goto _again;
	tr180: cs = 128; goto _again;
	tr181: cs = 129; goto _again;
	tr182: cs = 130; goto _again;
	tr183: cs = 131; goto _again;
	tr184: cs = 132; goto _again;
	tr185: cs = 133; goto _again;
	tr186: cs = 134; goto _again;
	tr187: cs = 135; goto _again;
	tr188: cs = 136; goto _again;
	tr189: cs = 137; goto _again;
	tr190: cs = 138; goto _again;
	tr175: cs = 139; goto _again;
	tr192: cs = 140; goto _again;
	tr193: cs = 141; goto _again;
	tr1055: cs = 142; goto _again;
	tr195: cs = 143; goto _again;
	tr196: cs = 144; goto _again;
	tr198: cs = 145; goto _again;
	tr199: cs = 146; goto _again;
	tr200: cs = 147; goto _again;
	tr201: cs = 148; goto _again;
	tr1062: cs = 149; goto _again;
	tr204: cs = 150; goto _again;
	tr205: cs = 151; goto _again;
	tr1063: cs = 152; goto _again;
	tr207: cs = 153; goto _again;
	tr208: cs = 154; goto _again;
	tr1059: cs = 155; goto _again;
	tr210: cs = 156; goto _again;
	tr211: cs = 157; goto _again;
	tr212: cs = 158; goto _again;
	tr213: cs = 159; goto _again;
	tr214: cs = 160; goto _again;
	tr215: cs = 161; goto _again;
	tr1060: cs = 162; goto _again;
	tr217: cs = 163; goto _again;
	tr219: cs = 164; goto _again;
	tr220: cs = 165; goto _again;
	tr221: cs = 166; goto _again;
	tr218: cs = 167; goto _again;
	tr223: cs = 168; goto _again;
	tr224: cs = 169; goto _again;
	tr225: cs = 170; goto _again;
	tr226: cs = 171; goto _again;
	tr1064: cs = 172; goto _again;
	tr228: cs = 173; goto _again;
	tr230: cs = 174; goto _again;
	tr231: cs = 175; goto _again;
	tr232: cs = 176; goto _again;
	tr229: cs = 177; goto _again;
	tr234: cs = 178; goto _again;
	tr235: cs = 179; goto _again;
	tr236: cs = 180; goto _again;
	tr237: cs = 181; goto _again;
	tr238: cs = 182; goto _again;
	tr239: cs = 183; goto _again;
	tr1065: cs = 184; goto _again;
	tr241: cs = 185; goto _again;
	tr242: cs = 186; goto _again;
	tr243: cs = 187; goto _again;
	tr244: cs = 188; goto _again;
	tr245: cs = 189; goto _again;
	tr246: cs = 190; goto _again;
	tr247: cs = 191; goto _again;
	tr248: cs = 192; goto _again;
	tr249: cs = 193; goto _again;
	tr250: cs = 194; goto _again;
	tr251: cs = 195; goto _again;
	tr252: cs = 196; goto _again;
	tr253: cs = 197; goto _again;
	tr254: cs = 198; goto _again;
	tr255: cs = 199; goto _again;
	tr256: cs = 200; goto _again;
	tr257: cs = 201; goto _again;
	tr1066: cs = 202; goto _again;
	tr259: cs = 203; goto _again;
	tr260: cs = 204; goto _again;
	tr261: cs = 205; goto _again;
	tr262: cs = 206; goto _again;
	tr263: cs = 207; goto _again;
	tr1067: cs = 208; goto _again;
	tr265: cs = 209; goto _again;
	tr266: cs = 210; goto _again;
	tr267: cs = 211; goto _again;
	tr268: cs = 212; goto _again;
	tr269: cs = 213; goto _again;
	tr1068: cs = 214; goto _again;
	tr271: cs = 215; goto _again;
	tr272: cs = 216; goto _again;
	tr273: cs = 217; goto _again;
	tr274: cs = 218; goto _again;
	tr275: cs = 219; goto _again;
	tr276: cs = 220; goto _again;
	tr277: cs = 221; goto _again;
	tr1069: cs = 222; goto _again;
	tr279: cs = 223; goto _again;
	tr280: cs = 224; goto _again;
	tr281: cs = 225; goto _again;
	tr1070: cs = 226; goto _again;
	tr283: cs = 227; goto _again;
	tr284: cs = 228; goto _again;
	tr1071: cs = 229; goto _again;
	tr286: cs = 230; goto _again;
	tr288: cs = 231; goto _again;
	tr289: cs = 232; goto _again;
	tr290: cs = 233; goto _again;
	tr291: cs = 234; goto _again;
	tr287: cs = 235; goto _again;
	tr293: cs = 236; goto _again;
	tr294: cs = 237; goto _again;
	tr295: cs = 238; goto _again;
	tr296: cs = 239; goto _again;
	tr1072: cs = 240; goto _again;
	tr1076: cs = 241; goto _again;
	tr300: cs = 242; goto _again;
	tr1077: cs = 243; goto _again;
	tr302: cs = 244; goto _again;
	tr303: cs = 245; goto _again;
	tr1073: cs = 246; goto _again;
	tr305: cs = 247; goto _again;
	tr306: cs = 248; goto _again;
	tr307: cs = 249; goto _again;
	tr1074: cs = 250; goto _again;
	tr309: cs = 251; goto _again;
	tr310: cs = 252; goto _again;
	tr311: cs = 253; goto _again;
	tr312: cs = 254; goto _again;
	tr313: cs = 255; goto _again;
	tr1078: cs = 256; goto _again;
	tr315: cs = 257; goto _again;
	tr316: cs = 258; goto _again;
	tr317: cs = 259; goto _again;
	tr318: cs = 260; goto _again;
	tr319: cs = 261; goto _again;
	tr320: cs = 262; goto _again;
	tr321: cs = 263; goto _again;
	tr322: cs = 264; goto _again;
	tr323: cs = 265; goto _again;
	tr324: cs = 266; goto _again;
	tr325: cs = 267; goto _again;
	tr326: cs = 268; goto _again;
	tr327: cs = 269; goto _again;
	tr1079: cs = 270; goto _again;
	tr329: cs = 271; goto _again;
	tr331: cs = 272; goto _again;
	tr332: cs = 273; goto _again;
	tr333: cs = 274; goto _again;
	tr334: cs = 275; goto _again;
	tr335: cs = 276; goto _again;
	tr330: cs = 277; goto _again;
	tr337: cs = 278; goto _again;
	tr338: cs = 279; goto _again;
	tr339: cs = 280; goto _again;
	tr340: cs = 281; goto _again;
	tr341: cs = 282; goto _again;
	tr342: cs = 283; goto _again;
	tr343: cs = 284; goto _again;
	tr344: cs = 285; goto _again;
	tr345: cs = 286; goto _again;
	tr346: cs = 287; goto _again;
	tr347: cs = 288; goto _again;
	tr348: cs = 289; goto _again;
	tr349: cs = 290; goto _again;
	tr350: cs = 291; goto _again;
	tr352: cs = 292; goto _again;
	tr353: cs = 293; goto _again;
	tr354: cs = 294; goto _again;
	tr351: cs = 295; goto _again;
	tr356: cs = 296; goto _again;
	tr357: cs = 297; goto _again;
	tr358: cs = 298; goto _again;
	tr359: cs = 299; goto _again;
	tr1080: cs = 300; goto _again;
	tr361: cs = 301; goto _again;
	tr362: cs = 302; goto _again;
	tr363: cs = 303; goto _again;
	tr364: cs = 304; goto _again;
	tr365: cs = 305; goto _again;
	tr1081: cs = 306; goto _again;
	tr367: cs = 307; goto _again;
	tr371: cs = 308; goto _again;
	tr372: cs = 309; goto _again;
	tr368: cs = 310; goto _again;
	tr374: cs = 311; goto _again;
	tr375: cs = 312; goto _again;
	tr376: cs = 313; goto _again;
	tr369: cs = 314; goto _again;
	tr378: cs = 315; goto _again;
	tr379: cs = 316; goto _again;
	tr380: cs = 317; goto _again;
	tr381: cs = 318; goto _again;
	tr370: cs = 319; goto _again;
	tr383: cs = 320; goto _again;
	tr384: cs = 321; goto _again;
	tr385: cs = 322; goto _again;
	tr386: cs = 323; goto _again;
	tr1082: cs = 324; goto _again;
	tr388: cs = 325; goto _again;
	tr390: cs = 326; goto _again;
	tr391: cs = 327; goto _again;
	tr392: cs = 328; goto _again;
	tr393: cs = 329; goto _again;
	tr394: cs = 330; goto _again;
	tr395: cs = 331; goto _again;
	tr389: cs = 332; goto _again;
	tr397: cs = 333; goto _again;
	tr1085: cs = 334; goto _again;
	tr401: cs = 335; goto _again;
	tr402: cs = 336; goto _again;
	tr1086: cs = 337; goto _again;
	tr404: cs = 338; goto _again;
	tr405: cs = 339; goto _again;
	tr406: cs = 340; goto _again;
	tr1087: cs = 341; goto _again;
	tr408: cs = 342; goto _again;
	tr411: cs = 343; goto _again;
	tr409: cs = 344; goto _again;
	tr413: cs = 345; goto _again;
	tr414: cs = 346; goto _again;
	tr415: cs = 347; goto _again;
	tr416: cs = 348; goto _again;
	tr410: cs = 349; goto _again;
	tr1093: cs = 350; goto _again;
	tr419: cs = 351; goto _again;
	tr421: cs = 352; goto _again;
	tr422: cs = 353; goto _again;
	tr420: cs = 354; goto _again;
	tr424: cs = 355; goto _again;
	tr425: cs = 356; goto _again;
	tr1095: cs = 357; goto _again;
	tr428: cs = 358; goto _again;
	tr430: cs = 359; goto _again;
	tr431: cs = 360; goto _again;
	tr432: cs = 361; goto _again;
	tr433: cs = 362; goto _again;
	tr434: cs = 363; goto _again;
	tr429: cs = 364; goto _again;
	tr436: cs = 365; goto _again;
	tr437: cs = 366; goto _again;
	tr438: cs = 367; goto _again;
	tr1102: cs = 368; goto _again;
	tr441: cs = 369; goto _again;
	tr442: cs = 370; goto _again;
	tr443: cs = 371; goto _again;
	tr444: cs = 372; goto _again;
	tr445: cs = 373; goto _again;
	tr446: cs = 374; goto _again;
	tr447: cs = 375; goto _again;
	tr448: cs = 376; goto _again;
	tr1099: cs = 377; goto _again;
	tr450: cs = 378; goto _again;
	tr451: cs = 379; goto _again;
	tr452: cs = 380; goto _again;
	tr453: cs = 381; goto _again;
	tr454: cs = 382; goto _again;
	tr455: cs = 383; goto _again;
	tr1100: cs = 384; goto _again;
	tr457: cs = 385; goto _again;
	tr458: cs = 386; goto _again;
	tr459: cs = 387; goto _again;
	tr460: cs = 388; goto _again;
	tr1105: cs = 389; goto _again;
	tr463: cs = 390; goto _again;
	tr464: cs = 391; goto _again;
	tr465: cs = 392; goto _again;
	tr466: cs = 393; goto _again;
	tr467: cs = 394; goto _again;
	tr468: cs = 395; goto _again;
	tr469: cs = 396; goto _again;
	tr470: cs = 397; goto _again;
	tr1106: cs = 398; goto _again;
	tr1109: cs = 399; goto _again;
	tr473: cs = 400; goto _again;
	tr474: cs = 401; goto _again;
	tr1110: cs = 402; goto _again;
	tr476: cs = 403; goto _again;
	tr478: cs = 404; goto _again;
	tr479: cs = 405; goto _again;
	tr480: cs = 406; goto _again;
	tr481: cs = 407; goto _again;
	tr477: cs = 408; goto _again;
	tr483: cs = 409; goto _again;
	tr484: cs = 410; goto _again;
	tr488: cs = 411; goto _again;
	tr489: cs = 412; goto _again;
	tr490: cs = 413; goto _again;
	tr491: cs = 414; goto _again;
	tr485: cs = 415; goto _again;
	tr493: cs = 416; goto _again;
	tr494: cs = 417; goto _again;
	tr495: cs = 418; goto _again;
	tr496: cs = 419; goto _again;
	tr497: cs = 420; goto _again;
	tr486: cs = 421; goto _again;
	tr499: cs = 422; goto _again;
	tr500: cs = 423; goto _again;
	tr501: cs = 424; goto _again;
	tr502: cs = 425; goto _again;
	tr503: cs = 426; goto _again;
	tr504: cs = 427; goto _again;
	tr505: cs = 428; goto _again;
	tr506: cs = 429; goto _again;
	tr507: cs = 430; goto _again;
	tr508: cs = 431; goto _again;
	tr509: cs = 432; goto _again;
	tr487: cs = 433; goto _again;
	tr511: cs = 434; goto _again;
	tr512: cs = 435; goto _again;
	tr513: cs = 436; goto _again;
	tr514: cs = 437; goto _again;
	tr1111: cs = 438; goto _again;
	tr516: cs = 439; goto _again;
	tr517: cs = 440; goto _again;
	tr1112: cs = 441; goto _again;
	tr519: cs = 442; goto _again;
	tr520: cs = 443; goto _again;
	tr521: cs = 444; goto _again;
	tr522: cs = 445; goto _again;
	tr1118: cs = 446; goto _again;
	tr525: cs = 447; goto _again;
	tr527: cs = 448; goto _again;
	tr528: cs = 449; goto _again;
	tr529: cs = 450; goto _again;
	tr530: cs = 451; goto _again;
	tr526: cs = 452; goto _again;
	tr532: cs = 453; goto _again;
	tr533: cs = 454; goto _again;
	tr534: cs = 455; goto _again;
	tr535: cs = 456; goto _again;
	tr536: cs = 457; goto _again;
	tr537: cs = 458; goto _again;
	tr1122: cs = 459; goto _again;
	tr539: cs = 460; goto _again;
	tr540: cs = 461; goto _again;
	tr541: cs = 462; goto _again;
	tr1123: cs = 463; goto _again;
	tr543: cs = 464; goto _again;
	tr544: cs = 465; goto _again;
	tr1125: cs = 466; goto _again;
	tr547: cs = 467; goto _again;
	tr549: cs = 468; goto _again;
	tr550: cs = 469; goto _again;
	tr551: cs = 470; goto _again;
	tr552: cs = 471; goto _again;
	tr553: cs = 472; goto _again;
	tr548: cs = 473; goto _again;
	tr555: cs = 474; goto _again;
	tr556: cs = 475; goto _again;
	tr557: cs = 476; goto _again;
	tr558: cs = 477; goto _again;
	tr559: cs = 478; goto _again;
	tr560: cs = 479; goto _again;
	tr1127: cs = 480; goto _again;
	tr562: cs = 481; goto _again;
	tr563: cs = 482; goto _again;
	tr564: cs = 483; goto _again;
	tr565: cs = 484; goto _again;
	tr1128: cs = 485; goto _again;
	tr567: cs = 486; goto _again;
	tr568: cs = 487; goto _again;
	tr569: cs = 488; goto _again;
	tr570: cs = 489; goto _again;
	tr1132: cs = 490; goto _again;
	tr572: cs = 491; goto _again;
	tr573: cs = 492; goto _again;
	tr574: cs = 493; goto _again;
	tr575: cs = 494; goto _again;
	tr576: cs = 495; goto _again;
	tr577: cs = 496; goto _again;
	tr1133: cs = 497; goto _again;
	tr579: cs = 498; goto _again;
	tr581: cs = 499; goto _again;
	tr582: cs = 500; goto _again;
	tr583: cs = 501; goto _again;
	tr584: cs = 502; goto _again;
	tr585: cs = 503; goto _again;
	tr586: cs = 504; goto _again;
	tr587: cs = 505; goto _again;
	tr588: cs = 506; goto _again;
	tr580: cs = 507; goto _again;
	tr590: cs = 508; goto _again;
	tr591: cs = 509; goto _again;
	tr1134: cs = 510; goto _again;
	tr593: cs = 511; goto _again;
	tr596: cs = 512; goto _again;
	tr598: cs = 513; goto _again;
	tr599: cs = 514; goto _again;
	tr597: cs = 515; goto _again;
	tr601: cs = 516; goto _again;
	tr602: cs = 517; goto _again;
	tr603: cs = 518; goto _again;
	tr594: cs = 519; goto _again;
	tr605: cs = 520; goto _again;
	tr606: cs = 521; goto _again;
	tr607: cs = 522; goto _again;
	tr610: cs = 523; goto _again;
	tr611: cs = 524; goto _again;
	tr608: cs = 525; goto _again;
	tr613: cs = 526; goto _again;
	tr614: cs = 527; goto _again;
	tr595: cs = 528; goto _again;
	tr616: cs = 529; goto _again;
	tr1135: cs = 530; goto _again;
	tr618: cs = 531; goto _again;
	tr619: cs = 532; goto _again;
	tr620: cs = 533; goto _again;
	tr1136: cs = 534; goto _again;
	tr622: cs = 535; goto _again;
	tr623: cs = 536; goto _again;
	tr625: cs = 537; goto _again;
	tr1137: cs = 538; goto _again;
	tr627: cs = 539; goto _again;
	tr629: cs = 540; goto _again;
	tr630: cs = 541; goto _again;
	tr631: cs = 542; goto _again;
	tr628: cs = 543; goto _again;
	tr633: cs = 544; goto _again;
	tr634: cs = 545; goto _again;
	tr635: cs = 546; goto _again;
	tr636: cs = 547; goto _again;
	tr1138: cs = 548; goto _again;
	tr638: cs = 549; goto _again;
	tr639: cs = 550; goto _again;
	tr640: cs = 551; goto _again;
	tr641: cs = 552; goto _again;
	tr642: cs = 553; goto _again;
	tr1139: cs = 554; goto _again;
	tr1140: cs = 555; goto _again;
	tr1141: cs = 556; goto _again;
	tr1142: cs = 557; goto _again;
	tr1143: cs = 558; goto _again;
	tr650: cs = 560; goto _again;
	tr651: cs = 561; goto f175;
	tr652: cs = 562; goto f176;
	tr655: cs = 563; goto _again;
	tr656: cs = 564; goto _again;
	tr657: cs = 565; goto _again;
	tr1167: cs = 566; goto _again;
	tr659: cs = 567; goto _again;
	tr660: cs = 568; goto _again;
	tr661: cs = 569; goto _again;
	tr662: cs = 570; goto _again;
	tr675: cs = 571; goto _again;
	tr686: cs = 572; goto _again;
	tr688: cs = 573; goto _again;
	tr690: cs = 574; goto _again;
	tr691: cs = 575; goto _again;
	tr692: cs = 576; goto _again;
	tr689: cs = 577; goto _again;
	tr694: cs = 578; goto _again;
	tr695: cs = 579; goto _again;
	tr696: cs = 580; goto _again;
	tr687: cs = 581; goto _again;
	tr698: cs = 582; goto _again;
	tr699: cs = 583; goto _again;
	tr700: cs = 584; goto _again;
	tr701: cs = 585; goto _again;
	tr676: cs = 586; goto _again;
	tr703: cs = 587; goto _again;
	tr704: cs = 588; goto _again;
	tr705: cs = 589; goto _again;
	tr706: cs = 590; goto _again;
	tr707: cs = 591; goto _again;
	tr677: cs = 592; goto _again;
	tr709: cs = 593; goto _again;
	tr710: cs = 594; goto _again;
	tr711: cs = 595; goto _again;
	tr712: cs = 596; goto _again;
	tr713: cs = 597; goto _again;
	tr678: cs = 598; goto _again;
	tr715: cs = 599; goto _again;
	tr716: cs = 600; goto _again;
	tr717: cs = 601; goto _again;
	tr718: cs = 602; goto _again;
	tr719: cs = 603; goto _again;
	tr679: cs = 604; goto _again;
	tr721: cs = 605; goto _again;
	tr722: cs = 606; goto _again;
	tr723: cs = 607; goto _again;
	tr724: cs = 608; goto _again;
	tr725: cs = 609; goto _again;
	tr680: cs = 610; goto _again;
	tr727: cs = 611; goto _again;
	tr728: cs = 612; goto _again;
	tr729: cs = 613; goto _again;
	tr730: cs = 614; goto _again;
	tr731: cs = 615; goto _again;
	tr681: cs = 616; goto _again;
	tr733: cs = 617; goto _again;
	tr735: cs = 618; goto _again;
	tr736: cs = 619; goto _again;
	tr737: cs = 620; goto _again;
	tr738: cs = 621; goto _again;
	tr734: cs = 622; goto _again;
	tr740: cs = 623; goto _again;
	tr741: cs = 624; goto _again;
	tr742: cs = 625; goto _again;
	tr743: cs = 626; goto _again;
	tr682: cs = 627; goto _again;
	tr745: cs = 628; goto _again;
	tr746: cs = 629; goto _again;
	tr747: cs = 630; goto _again;
	tr748: cs = 631; goto _again;
	tr749: cs = 632; goto _again;
	tr683: cs = 633; goto _again;
	tr751: cs = 634; goto _again;
	tr752: cs = 635; goto _again;
	tr753: cs = 636; goto _again;
	tr754: cs = 637; goto _again;
	tr755: cs = 638; goto _again;
	tr684: cs = 639; goto _again;
	tr757: cs = 640; goto _again;
	tr758: cs = 641; goto _again;
	tr759: cs = 642; goto _again;
	tr760: cs = 643; goto _again;
	tr685: cs = 644; goto _again;
	tr762: cs = 645; goto _again;
	tr763: cs = 646; goto _again;
	tr764: cs = 647; goto _again;
	tr765: cs = 648; goto _again;
	tr766: cs = 649; goto _again;
	tr767: cs = 650; goto _again;
	tr663: cs = 651; goto _again;
	tr769: cs = 652; goto _again;
	tr771: cs = 653; goto _again;
	tr773: cs = 654; goto _again;
	tr774: cs = 655; goto _again;
	tr775: cs = 656; goto _again;
	tr772: cs = 657; goto _again;
	tr777: cs = 658; goto _again;
	tr778: cs = 659; goto _again;
	tr779: cs = 660; goto _again;
	tr770: cs = 661; goto _again;
	tr781: cs = 662; goto _again;
	tr782: cs = 663; goto _again;
	tr783: cs = 664; goto _again;
	tr784: cs = 665; goto _again;
	tr664: cs = 666; goto _again;
	tr786: cs = 667; goto _again;
	tr787: cs = 668; goto _again;
	tr788: cs = 669; goto _again;
	tr789: cs = 670; goto _again;
	tr790: cs = 671; goto _again;
	tr665: cs = 672; goto _again;
	tr792: cs = 673; goto _again;
	tr793: cs = 674; goto _again;
	tr794: cs = 675; goto _again;
	tr795: cs = 676; goto _again;
	tr796: cs = 677; goto _again;
	tr666: cs = 678; goto _again;
	tr798: cs = 679; goto _again;
	tr799: cs = 680; goto _again;
	tr800: cs = 681; goto _again;
	tr801: cs = 682; goto _again;
	tr802: cs = 683; goto _again;
	tr667: cs = 684; goto _again;
	tr804: cs = 685; goto _again;
	tr805: cs = 686; goto _again;
	tr806: cs = 687; goto _again;
	tr807: cs = 688; goto _again;
	tr808: cs = 689; goto _again;
	tr668: cs = 690; goto _again;
	tr810: cs = 691; goto _again;
	tr811: cs = 692; goto _again;
	tr812: cs = 693; goto _again;
	tr813: cs = 694; goto _again;
	tr814: cs = 695; goto _again;
	tr669: cs = 696; goto _again;
	tr816: cs = 697; goto _again;
	tr818: cs = 698; goto _again;
	tr819: cs = 699; goto _again;
	tr820: cs = 700; goto _again;
	tr821: cs = 701; goto _again;
	tr817: cs = 702; goto _again;
	tr823: cs = 703; goto _again;
	tr824: cs = 704; goto _again;
	tr825: cs = 705; goto _again;
	tr826: cs = 706; goto _again;
	tr670: cs = 707; goto _again;
	tr828: cs = 708; goto _again;
	tr829: cs = 709; goto _again;
	tr830: cs = 710; goto _again;
	tr831: cs = 711; goto _again;
	tr832: cs = 712; goto _again;
	tr671: cs = 713; goto _again;
	tr834: cs = 714; goto _again;
	tr835: cs = 715; goto _again;
	tr836: cs = 716; goto _again;
	tr837: cs = 717; goto _again;
	tr838: cs = 718; goto _again;
	tr672: cs = 719; goto _again;
	tr840: cs = 720; goto _again;
	tr841: cs = 721; goto _again;
	tr842: cs = 722; goto _again;
	tr843: cs = 723; goto _again;
	tr673: cs = 724; goto _again;
	tr845: cs = 725; goto _again;
	tr846: cs = 726; goto _again;
	tr847: cs = 727; goto _again;
	tr848: cs = 728; goto _again;
	tr849: cs = 729; goto _again;
	tr850: cs = 730; goto _again;
	tr852: cs = 731; goto _again;
	tr853: cs = 732; goto _again;
	tr854: cs = 733; goto _again;
	tr1211: cs = 734; goto _again;
	tr856: cs = 735; goto _again;
	tr859: cs = 736; goto _again;
	tr1226: cs = 737; goto _again;
	tr1227: cs = 738; goto _again;
	tr863: cs = 739; goto _again;
	tr1245: cs = 740; goto _again;
	tr1246: cs = 741; goto _again;
	tr867: cs = 742; goto _again;
	tr1257: cs = 743; goto _again;
	tr1258: cs = 744; goto _again;
	tr871: cs = 745; goto _again;
	tr0: cs = 746; goto f0;
	tr1: cs = 746; goto f1;
	tr2: cs = 746; goto f2;
	tr3: cs = 746; goto f3;
	tr4: cs = 746; goto f4;
	tr8: cs = 746; goto f7;
	tr12: cs = 746; goto f9;
	tr14: cs = 746; goto f10;
	tr15: cs = 746; goto f11;
	tr20: cs = 746; goto f13;
	tr21: cs = 746; goto f14;
	tr22: cs = 746; goto f15;
	tr24: cs = 746; goto f16;
	tr26: cs = 746; goto f18;
	tr29: cs = 746; goto f19;
	tr31: cs = 746; goto f20;
	tr34: cs = 746; goto f22;
	tr36: cs = 746; goto f23;
	tr37: cs = 746; goto f24;
	tr38: cs = 746; goto f25;
	tr40: cs = 746; goto f26;
	tr43: cs = 746; goto f27;
	tr45: cs = 746; goto f28;
	tr46: cs = 746; goto f29;
	tr49: cs = 746; goto f31;
	tr52: cs = 746; goto f32;
	tr55: cs = 746; goto f34;
	tr59: cs = 746; goto f35;
	tr63: cs = 746; goto f36;
	tr67: cs = 746; goto f37;
	tr69: cs = 746; goto f38;
	tr77: cs = 746; goto f41;
	tr80: cs = 746; goto f42;
	tr82: cs = 746; goto f43;
	tr83: cs = 746; goto f44;
	tr85: cs = 746; goto f45;
	tr87: cs = 746; goto f46;
	tr89: cs = 746; goto f47;
	tr90: cs = 746; goto f48;
	tr92: cs = 746; goto f49;
	tr93: cs = 746; goto f50;
	tr95: cs = 746; goto f51;
	tr96: cs = 746; goto f52;
	tr103: cs = 746; goto f57;
	tr104: cs = 746; goto f58;
	tr106: cs = 746; goto f59;
	tr873: cs = 746; goto f223;
	tr874: cs = 746; goto f224;
	tr875: cs = 746; goto f225;
	tr876: cs = 746; goto f226;
	tr879: cs = 746; goto f227;
	tr882: cs = 746; goto f228;
	tr886: cs = 746; goto f229;
	tr888: cs = 746; goto f231;
	tr889: cs = 746; goto f232;
	tr893: cs = 746; goto f233;
	tr894: cs = 746; goto f234;
	tr897: cs = 746; goto f235;
	tr898: cs = 746; goto f236;
	tr905: cs = 746; goto f239;
	tr906: cs = 746; goto f240;
	tr907: cs = 746; goto f241;
	tr911: cs = 746; goto f242;
	tr912: cs = 746; goto f243;
	tr913: cs = 746; goto f244;
	tr914: cs = 746; goto f245;
	tr915: cs = 746; goto f246;
	tr916: cs = 746; goto f247;
	tr917: cs = 746; goto f248;
	tr918: cs = 746; goto f249;
	tr919: cs = 746; goto f250;
	tr920: cs = 746; goto f251;
	tr921: cs = 746; goto f252;
	tr922: cs = 746; goto f253;
	tr926: cs = 746; goto f257;
	tr927: cs = 746; goto f258;
	tr928: cs = 746; goto f259;
	tr929: cs = 746; goto f260;
	tr930: cs = 746; goto f261;
	tr931: cs = 746; goto f262;
	tr932: cs = 746; goto f263;
	tr933: cs = 746; goto f264;
	tr934: cs = 746; goto f265;
	tr936: cs = 746; goto f266;
	tr937: cs = 746; goto f267;
	tr938: cs = 746; goto f268;
	tr939: cs = 746; goto f269;
	tr940: cs = 746; goto f270;
	tr941: cs = 746; goto f271;
	tr942: cs = 746; goto f272;
	tr943: cs = 746; goto f273;
	tr944: cs = 746; goto f274;
	tr946: cs = 746; goto f275;
	tr947: cs = 746; goto f276;
	tr948: cs = 746; goto f277;
	tr950: cs = 746; goto f279;
	tr952: cs = 746; goto f280;
	tr955: cs = 746; goto f281;
	tr956: cs = 746; goto f282;
	tr957: cs = 746; goto f283;
	tr958: cs = 746; goto f284;
	tr959: cs = 746; goto f285;
	tr961: cs = 746; goto f286;
	tr962: cs = 746; goto f287;
	tr964: cs = 746; goto f289;
	tr965: cs = 746; goto f290;
	tr968: cs = 746; goto f293;
	tr970: cs = 746; goto f295;
	tr971: cs = 746; goto f296;
	tr972: cs = 746; goto f297;
	tr973: cs = 746; goto f298;
	tr974: cs = 746; goto f299;
	tr975: cs = 746; goto f300;
	tr976: cs = 746; goto f301;
	tr980: cs = 746; goto f303;
	tr982: cs = 746; goto f304;
	tr983: cs = 746; goto f305;
	tr985: cs = 746; goto f306;
	tr986: cs = 746; goto f307;
	tr990: cs = 746; goto f308;
	tr992: cs = 746; goto f309;
	tr993: cs = 746; goto f310;
	tr994: cs = 746; goto f311;
	tr995: cs = 746; goto f312;
	tr1000: cs = 746; goto f316;
	tr1001: cs = 746; goto f317;
	tr1002: cs = 746; goto f318;
	tr1003: cs = 746; goto f319;
	tr1004: cs = 746; goto f320;
	tr1005: cs = 746; goto f321;
	tr1006: cs = 746; goto f322;
	tr1007: cs = 746; goto f323;
	tr1008: cs = 746; goto f324;
	tr1009: cs = 746; goto f325;
	tr878: cs = 747; goto f5;
	tr5: cs = 748; goto f5;
	tr6: cs = 749; goto f6;
	tr7: cs = 750; goto f6;
	tr9: cs = 751; goto f5;
	tr10: cs = 752; goto f8;
	tr11: cs = 753; goto f5;
	tr13: cs = 754; goto f6;
	tr16: cs = 755; goto _again;
	tr17: cs = 756; goto f5;
	tr18: cs = 757; goto f5;
	tr19: cs = 758; goto f12;
	tr880: cs = 759; goto _again;
	tr881: cs = 760; goto _again;
	tr883: cs = 761; goto _again;
	tr884: cs = 762; goto f5;
	tr885: cs = 763; goto _again;
	tr923: cs = 764; goto f254;
	tr963: cs = 765; goto f288;
	tr924: cs = 766; goto f255;
	tr966: cs = 767; goto f291;
	tr925: cs = 768; goto f256;
	tr967: cs = 768; goto f292;
	tr969: cs = 768; goto f294;
	tr935: cs = 769; goto _again;
	tr945: cs = 770; goto _again;
	tr949: cs = 771; goto f278;
	tr70: cs = 772; goto f39;
	tr981: cs = 773; goto f39;
	tr978: cs = 774; goto f39;
	tr984: cs = 775; goto f39;
	tr951: cs = 776; goto f5;
	tr953: cs = 777; goto f5;
	tr954: cs = 778; goto _again;
	tr960: cs = 779; goto f40;
	tr996: cs = 780; goto f313;
	tr997: cs = 780; goto f314;
	tr998: cs = 780; goto f315;
	tr999: cs = 781; goto f5;
	tr887: cs = 782; goto f230;
	tr102: cs = 783; goto _again;
	tr99: cs = 783; goto f54;
	tr101: cs = 783; goto f56;
	tr890: cs = 784; goto _again;
	tr891: cs = 785; goto f5;
	tr892: cs = 786; goto f5;
	tr107: cs = 787; goto f60;
	tr109: cs = 787; goto f61;
	tr111: cs = 787; goto f62;
	tr113: cs = 787; goto f63;
	tr115: cs = 787; goto f64;
	tr1013: cs = 787; goto f326;
	tr1015: cs = 787; goto f327;
	tr1012: cs = 788; goto f5;
	tr1014: cs = 789; goto f5;
	tr116: cs = 790; goto f65;
	tr117: cs = 790; goto f66;
	tr122: cs = 790; goto f67;
	tr127: cs = 790; goto f68;
	tr132: cs = 790; goto f69;
	tr140: cs = 790; goto f70;
	tr142: cs = 790; goto f71;
	tr144: cs = 790; goto f72;
	tr149: cs = 790; goto f73;
	tr155: cs = 790; goto f74;
	tr160: cs = 790; goto f75;
	tr163: cs = 790; goto f76;
	tr170: cs = 790; goto f77;
	tr172: cs = 790; goto f78;
	tr173: cs = 790; goto f79;
	tr191: cs = 790; goto f80;
	tr194: cs = 790; goto f81;
	tr197: cs = 790; goto f82;
	tr202: cs = 790; goto f83;
	tr203: cs = 790; goto f84;
	tr206: cs = 790; goto f85;
	tr209: cs = 790; goto f86;
	tr216: cs = 790; goto f87;
	tr222: cs = 790; goto f88;
	tr227: cs = 790; goto f89;
	tr233: cs = 790; goto f90;
	tr240: cs = 790; goto f91;
	tr258: cs = 790; goto f92;
	tr264: cs = 790; goto f93;
	tr270: cs = 790; goto f94;
	tr278: cs = 790; goto f95;
	tr282: cs = 790; goto f96;
	tr285: cs = 790; goto f97;
	tr292: cs = 790; goto f98;
	tr297: cs = 790; goto f99;
	tr299: cs = 790; goto f100;
	tr301: cs = 790; goto f101;
	tr304: cs = 790; goto f102;
	tr308: cs = 790; goto f103;
	tr314: cs = 790; goto f104;
	tr328: cs = 790; goto f105;
	tr336: cs = 790; goto f106;
	tr355: cs = 790; goto f107;
	tr360: cs = 790; goto f108;
	tr366: cs = 790; goto f109;
	tr373: cs = 790; goto f110;
	tr377: cs = 790; goto f111;
	tr382: cs = 790; goto f112;
	tr387: cs = 790; goto f113;
	tr396: cs = 790; goto f114;
	tr398: cs = 790; goto f115;
	tr399: cs = 790; goto f116;
	tr400: cs = 790; goto f117;
	tr403: cs = 790; goto f118;
	tr407: cs = 790; goto f119;
	tr412: cs = 790; goto f120;
	tr417: cs = 790; goto f121;
	tr418: cs = 790; goto f122;
	tr423: cs = 790; goto f123;
	tr426: cs = 790; goto f124;
	tr427: cs = 790; goto f125;
	tr435: cs = 790; goto f126;
	tr439: cs = 790; goto f127;
	tr440: cs = 790; goto f128;
	tr449: cs = 790; goto f129;
	tr456: cs = 790; goto f130;
	tr461: cs = 790; goto f131;
	tr462: cs = 790; goto f132;
	tr471: cs = 790; goto f133;
	tr472: cs = 790; goto f134;
	tr475: cs = 790; goto f135;
	tr482: cs = 790; goto f136;
	tr492: cs = 790; goto f137;
	tr498: cs = 790; goto f138;
	tr510: cs = 790; goto f139;
	tr515: cs = 790; goto f140;
	tr518: cs = 790; goto f141;
	tr523: cs = 790; goto f142;
	tr524: cs = 790; goto f143;
	tr531: cs = 790; goto f144;
	tr538: cs = 790; goto f145;
	tr542: cs = 790; goto f146;
	tr545: cs = 790; goto f147;
	tr546: cs = 790; goto f148;
	tr554: cs = 790; goto f149;
	tr561: cs = 790; goto f150;
	tr566: cs = 790; goto f151;
	tr571: cs = 790; goto f152;
	tr578: cs = 790; goto f153;
	tr589: cs = 790; goto f154;
	tr592: cs = 790; goto f155;
	tr600: cs = 790; goto f156;
	tr604: cs = 790; goto f157;
	tr609: cs = 790; goto f158;
	tr612: cs = 790; goto f159;
	tr615: cs = 790; goto f160;
	tr617: cs = 790; goto f161;
	tr621: cs = 790; goto f162;
	tr624: cs = 790; goto f163;
	tr626: cs = 790; goto f164;
	tr632: cs = 790; goto f165;
	tr637: cs = 790; goto f166;
	tr643: cs = 790; goto f167;
	tr644: cs = 790; goto f168;
	tr645: cs = 790; goto f169;
	tr646: cs = 790; goto f170;
	tr647: cs = 790; goto f171;
	tr648: cs = 790; goto f172;
	tr1018: cs = 790; goto f328;
	tr1042: cs = 790; goto f329;
	tr1051: cs = 790; goto f330;
	tr1053: cs = 790; goto f331;
	tr1054: cs = 790; goto f332;
	tr1056: cs = 790; goto f333;
	tr1058: cs = 790; goto f334;
	tr1061: cs = 790; goto f335;
	tr1075: cs = 790; goto f336;
	tr1083: cs = 790; goto f337;
	tr1084: cs = 790; goto f338;
	tr1088: cs = 790; goto f339;
	tr1089: cs = 790; goto f340;
	tr1090: cs = 790; goto f341;
	tr1091: cs = 790; goto f342;
	tr1092: cs = 790; goto f343;
	tr1094: cs = 790; goto f344;
	tr1096: cs = 790; goto f345;
	tr1098: cs = 790; goto f346;
	tr1101: cs = 790; goto f347;
	tr1103: cs = 790; goto f348;
	tr1104: cs = 790; goto f349;
	tr1107: cs = 790; goto f350;
	tr1108: cs = 790; goto f351;
	tr1113: cs = 790; goto f352;
	tr1114: cs = 790; goto f353;
	tr1115: cs = 790; goto f354;
	tr1116: cs = 790; goto f355;
	tr1117: cs = 790; goto f356;
	tr1119: cs = 790; goto f357;
	tr1120: cs = 790; goto f358;
	tr1121: cs = 790; goto f359;
	tr1124: cs = 790; goto f360;
	tr1126: cs = 790; goto f361;
	tr1129: cs = 790; goto f362;
	tr1130: cs = 790; goto f363;
	tr1131: cs = 790; goto f364;
	tr1144: cs = 790; goto f365;
	tr1145: cs = 790; goto f366;
	tr1146: cs = 790; goto f367;
	tr1147: cs = 790; goto f368;
	tr1148: cs = 790; goto f369;
	tr1019: cs = 791; goto f5;
	tr1020: cs = 792; goto f5;
	tr1021: cs = 793; goto f5;
	tr1057: cs = 794; goto f5;
	tr1022: cs = 795; goto f5;
	tr1023: cs = 796; goto f5;
	tr1024: cs = 797; goto f5;
	tr1025: cs = 798; goto f5;
	tr298: cs = 799; goto f5;
	tr1026: cs = 800; goto f5;
	tr1027: cs = 801; goto f5;
	tr1028: cs = 802; goto f5;
	tr1029: cs = 803; goto f5;
	tr1030: cs = 804; goto f5;
	tr1097: cs = 805; goto f5;
	tr1031: cs = 806; goto f5;
	tr1032: cs = 807; goto f5;
	tr1033: cs = 808; goto f5;
	tr1034: cs = 809; goto f5;
	tr1035: cs = 810; goto f5;
	tr1036: cs = 811; goto f5;
	tr1037: cs = 812; goto f5;
	tr1038: cs = 813; goto f5;
	tr1039: cs = 814; goto f5;
	tr1040: cs = 815; goto _again;
	tr1041: cs = 816; goto _again;
	tr653: cs = 817; goto f177;
	tr1149: cs = 818; goto f370;
	tr1150: cs = 818; goto f371;
	tr1151: cs = 818; goto f372;
	tr1152: cs = 818; goto f373;
	tr1153: cs = 818; goto f374;
	tr1154: cs = 818; goto f375;
	tr1155: cs = 818; goto f376;
	tr1156: cs = 818; goto f377;
	tr654: cs = 819; goto f178;
	tr658: cs = 819; goto f179;
	tr674: cs = 819; goto f180;
	tr693: cs = 819; goto f181;
	tr697: cs = 819; goto f182;
	tr702: cs = 819; goto f183;
	tr708: cs = 819; goto f184;
	tr714: cs = 819; goto f185;
	tr720: cs = 819; goto f186;
	tr726: cs = 819; goto f187;
	tr732: cs = 819; goto f188;
	tr739: cs = 819; goto f189;
	tr744: cs = 819; goto f190;
	tr750: cs = 819; goto f191;
	tr756: cs = 819; goto f192;
	tr761: cs = 819; goto f193;
	tr768: cs = 819; goto f194;
	tr776: cs = 819; goto f195;
	tr780: cs = 819; goto f196;
	tr785: cs = 819; goto f197;
	tr791: cs = 819; goto f198;
	tr797: cs = 819; goto f199;
	tr803: cs = 819; goto f200;
	tr809: cs = 819; goto f201;
	tr815: cs = 819; goto f202;
	tr822: cs = 819; goto f203;
	tr827: cs = 819; goto f204;
	tr833: cs = 819; goto f205;
	tr839: cs = 819; goto f206;
	tr844: cs = 819; goto f207;
	tr851: cs = 819; goto f208;
	tr855: cs = 819; goto f209;
	tr857: cs = 819; goto f210;
	tr858: cs = 819; goto f211;
	tr860: cs = 819; goto f212;
	tr861: cs = 819; goto f213;
	tr862: cs = 819; goto f214;
	tr864: cs = 819; goto f215;
	tr1157: cs = 819; goto f378;
	tr1158: cs = 819; goto f379;
	tr1161: cs = 819; goto f380;
	tr1162: cs = 819; goto f381;
	tr1166: cs = 819; goto f382;
	tr1168: cs = 819; goto f383;
	tr1171: cs = 819; goto f385;
	tr1172: cs = 819; goto f386;
	tr1173: cs = 819; goto f387;
	tr1174: cs = 819; goto f388;
	tr1175: cs = 819; goto f389;
	tr1176: cs = 819; goto f390;
	tr1178: cs = 819; goto f391;
	tr1179: cs = 819; goto f392;
	tr1180: cs = 819; goto f393;
	tr1181: cs = 819; goto f394;
	tr1182: cs = 819; goto f395;
	tr1183: cs = 819; goto f396;
	tr1185: cs = 819; goto f397;
	tr1186: cs = 819; goto f398;
	tr1187: cs = 819; goto f399;
	tr1188: cs = 819; goto f400;
	tr1189: cs = 819; goto f401;
	tr1190: cs = 819; goto f402;
	tr1193: cs = 819; goto f403;
	tr1194: cs = 819; goto f404;
	tr1195: cs = 819; goto f405;
	tr1196: cs = 819; goto f406;
	tr1197: cs = 819; goto f407;
	tr1199: cs = 819; goto f408;
	tr1201: cs = 819; goto f409;
	tr1202: cs = 819; goto f410;
	tr1204: cs = 819; goto f411;
	tr1205: cs = 819; goto f412;
	tr1206: cs = 819; goto f413;
	tr1207: cs = 819; goto f414;
	tr1208: cs = 819; goto f415;
	tr1209: cs = 819; goto f416;
	tr1210: cs = 819; goto f417;
	tr1212: cs = 819; goto f418;
	tr1213: cs = 819; goto f419;
	tr1214: cs = 819; goto f420;
	tr1215: cs = 819; goto f421;
	tr1220: cs = 819; goto f422;
	tr1221: cs = 819; goto f423;
	tr1222: cs = 819; goto f424;
	tr1223: cs = 819; goto f425;
	tr1224: cs = 819; goto f426;
	tr1225: cs = 819; goto f427;
	tr1159: cs = 820; goto f5;
	tr1160: cs = 821; goto _again;
	tr1169: cs = 822; goto f254;
	tr1200: cs = 823; goto f288;
	tr1170: cs = 824; goto f384;
	tr1203: cs = 825; goto f288;
	tr1177: cs = 826; goto _again;
	tr1184: cs = 827; goto _again;
	tr1191: cs = 828; goto f5;
	tr1192: cs = 829; goto _again;
	tr1198: cs = 830; goto f40;
	tr1216: cs = 831; goto f313;
	tr1217: cs = 831; goto f314;
	tr1218: cs = 831; goto f315;
	tr1219: cs = 832; goto f5;
	tr1163: cs = 833; goto _again;
	tr1164: cs = 834; goto f5;
	tr1165: cs = 835; goto f5;
	tr1228: cs = 836; goto f428;
	tr1230: cs = 836; goto f429;
	tr1231: cs = 836; goto f430;
	tr1232: cs = 836; goto f431;
	tr1233: cs = 836; goto f432;
	tr1234: cs = 836; goto f433;
	tr1229: cs = 837; goto _again;
	tr865: cs = 838; goto f216;
	tr866: cs = 838; goto f217;
	tr868: cs = 838; goto f218;
	tr1235: cs = 838; goto f434;
	tr1237: cs = 838; goto f435;
	tr1241: cs = 838; goto f436;
	tr1242: cs = 838; goto f437;
	tr1243: cs = 838; goto f438;
	tr1244: cs = 838; goto f439;
	tr1236: cs = 839; goto _again;
	tr1238: cs = 840; goto _again;
	tr1239: cs = 841; goto f5;
	tr1240: cs = 842; goto f5;
	tr869: cs = 843; goto f219;
	tr870: cs = 843; goto f220;
	tr872: cs = 843; goto f221;
	tr1247: cs = 843; goto f440;
	tr1249: cs = 843; goto f441;
	tr1253: cs = 843; goto f442;
	tr1254: cs = 843; goto f443;
	tr1255: cs = 843; goto f444;
	tr1256: cs = 843; goto f445;
	tr1248: cs = 844; goto _again;
	tr1250: cs = 845; goto _again;
	tr1251: cs = 846; goto f5;
	tr1252: cs = 847; goto f5;
	tr1259: cs = 848; goto f446;
	tr1260: cs = 848; goto f447;
	tr1261: cs = 849; goto f448;
	tr1262: cs = 849; goto f449;

	f237: _acts = _regex_actions + 1; goto execFuncs;
	f17: _acts = _regex_actions + 3; goto execFuncs;
	f254: _acts = _regex_actions + 5; goto execFuncs;
	f40: _acts = _regex_actions + 7; goto execFuncs;
	f384: _acts = _regex_actions + 9; goto execFuncs;
	f53: _acts = _regex_actions + 11; goto execFuncs;
	f55: _acts = _regex_actions + 13; goto execFuncs;
	f288: _acts = _regex_actions + 15; goto execFuncs;
	f39: _acts = _regex_actions + 17; goto execFuncs;
	f313: _acts = _regex_actions + 19; goto execFuncs;
	f315: _acts = _regex_actions + 21; goto execFuncs;
	f314: _acts = _regex_actions + 23; goto execFuncs;
	f33: _acts = _regex_actions + 25; goto execFuncs;
	f30: _acts = _regex_actions + 27; goto execFuncs;
	f54: _acts = _regex_actions + 29; goto execFuncs;
	f56: _acts = _regex_actions + 31; goto execFuncs;
	f176: _acts = _regex_actions + 33; goto execFuncs;
	f175: _acts = _regex_actions + 35; goto execFuncs;
	f177: _acts = _regex_actions + 37; goto execFuncs;
	f174: _acts = _regex_actions + 39; goto execFuncs;
	f5: _acts = _regex_actions + 45; goto execFuncs;
	f64: _acts = _regex_actions + 47; goto execFuncs;
	f63: _acts = _regex_actions + 49; goto execFuncs;
	f62: _acts = _regex_actions + 51; goto execFuncs;
	f61: _acts = _regex_actions + 53; goto execFuncs;
	f326: _acts = _regex_actions + 55; goto execFuncs;
	f327: _acts = _regex_actions + 57; goto execFuncs;
	f60: _acts = _regex_actions + 59; goto execFuncs;
	f331: _acts = _regex_actions + 61; goto execFuncs;
	f332: _acts = _regex_actions + 63; goto execFuncs;
	f333: _acts = _regex_actions + 65; goto execFuncs;
	f334: _acts = _regex_actions + 67; goto execFuncs;
	f339: _acts = _regex_actions + 69; goto execFuncs;
	f340: _acts = _regex_actions + 71; goto execFuncs;
	f341: _acts = _regex_actions + 73; goto execFuncs;
	f342: _acts = _regex_actions + 75; goto execFuncs;
	f343: _acts = _regex_actions + 77; goto execFuncs;
	f338: _acts = _regex_actions + 79; goto execFuncs;
	f345: _acts = _regex_actions + 81; goto execFuncs;
	f346: _acts = _regex_actions + 83; goto execFuncs;
	f349: _acts = _regex_actions + 85; goto execFuncs;
	f350: _acts = _regex_actions + 87; goto execFuncs;
	f351: _acts = _regex_actions + 89; goto execFuncs;
	f353: _acts = _regex_actions + 91; goto execFuncs;
	f354: _acts = _regex_actions + 93; goto execFuncs;
	f355: _acts = _regex_actions + 95; goto execFuncs;
	f356: _acts = _regex_actions + 97; goto execFuncs;
	f357: _acts = _regex_actions + 99; goto execFuncs;
	f358: _acts = _regex_actions + 101; goto execFuncs;
	f359: _acts = _regex_actions + 103; goto execFuncs;
	f361: _acts = _regex_actions + 105; goto execFuncs;
	f362: _acts = _regex_actions + 107; goto execFuncs;
	f363: _acts = _regex_actions + 109; goto execFuncs;
	f364: _acts = _regex_actions + 111; goto execFuncs;
	f367: _acts = _regex_actions + 113; goto execFuncs;
	f368: _acts = _regex_actions + 115; goto execFuncs;
	f369: _acts = _regex_actions + 117; goto execFuncs;
	f169: _acts = _regex_actions + 119; goto execFuncs;
	f170: _acts = _regex_actions + 121; goto execFuncs;
	f171: _acts = _regex_actions + 123; goto execFuncs;
	f172: _acts = _regex_actions + 125; goto execFuncs;
	f67: _acts = _regex_actions + 127; goto execFuncs;
	f68: _acts = _regex_actions + 129; goto execFuncs;
	f69: _acts = _regex_actions + 131; goto execFuncs;
	f70: _acts = _regex_actions + 133; goto execFuncs;
	f71: _acts = _regex_actions + 135; goto execFuncs;
	f72: _acts = _regex_actions + 137; goto execFuncs;
	f73: _acts = _regex_actions + 139; goto execFuncs;
	f74: _acts = _regex_actions + 141; goto execFuncs;
	f75: _acts = _regex_actions + 143; goto execFuncs;
	f76: _acts = _regex_actions + 145; goto execFuncs;
	f77: _acts = _regex_actions + 147; goto execFuncs;
	f78: _acts = _regex_actions + 149; goto execFuncs;
	f80: _acts = _regex_actions + 151; goto execFuncs;
	f81: _acts = _regex_actions + 153; goto execFuncs;
	f82: _acts = _regex_actions + 155; goto execFuncs;
	f83: _acts = _regex_actions + 157; goto execFuncs;
	f85: _acts = _regex_actions + 159; goto execFuncs;
	f86: _acts = _regex_actions + 161; goto execFuncs;
	f87: _acts = _regex_actions + 163; goto execFuncs;
	f88: _acts = _regex_actions + 165; goto execFuncs;
	f89: _acts = _regex_actions + 167; goto execFuncs;
	f90: _acts = _regex_actions + 169; goto execFuncs;
	f91: _acts = _regex_actions + 171; goto execFuncs;
	f92: _acts = _regex_actions + 173; goto execFuncs;
	f93: _acts = _regex_actions + 175; goto execFuncs;
	f94: _acts = _regex_actions + 177; goto execFuncs;
	f95: _acts = _regex_actions + 179; goto execFuncs;
	f96: _acts = _regex_actions + 181; goto execFuncs;
	f97: _acts = _regex_actions + 183; goto execFuncs;
	f98: _acts = _regex_actions + 185; goto execFuncs;
	f99: _acts = _regex_actions + 187; goto execFuncs;
	f101: _acts = _regex_actions + 189; goto execFuncs;
	f102: _acts = _regex_actions + 191; goto execFuncs;
	f103: _acts = _regex_actions + 193; goto execFuncs;
	f104: _acts = _regex_actions + 195; goto execFuncs;
	f105: _acts = _regex_actions + 197; goto execFuncs;
	f106: _acts = _regex_actions + 199; goto execFuncs;
	f107: _acts = _regex_actions + 201; goto execFuncs;
	f108: _acts = _regex_actions + 203; goto execFuncs;
	f109: _acts = _regex_actions + 205; goto execFuncs;
	f110: _acts = _regex_actions + 207; goto execFuncs;
	f111: _acts = _regex_actions + 209; goto execFuncs;
	f112: _acts = _regex_actions + 211; goto execFuncs;
	f113: _acts = _regex_actions + 213; goto execFuncs;
	f114: _acts = _regex_actions + 215; goto execFuncs;
	f115: _acts = _regex_actions + 217; goto execFuncs;
	f117: _acts = _regex_actions + 219; goto execFuncs;
	f118: _acts = _regex_actions + 221; goto execFuncs;
	f119: _acts = _regex_actions + 223; goto execFuncs;
	f120: _acts = _regex_actions + 225; goto execFuncs;
	f121: _acts = _regex_actions + 227; goto execFuncs;
	f122: _acts = _regex_actions + 229; goto execFuncs;
	f123: _acts = _regex_actions + 231; goto execFuncs;
	f124: _acts = _regex_actions + 233; goto execFuncs;
	f126: _acts = _regex_actions + 235; goto execFuncs;
	f127: _acts = _regex_actions + 237; goto execFuncs;
	f129: _acts = _regex_actions + 239; goto execFuncs;
	f130: _acts = _regex_actions + 241; goto execFuncs;
	f131: _acts = _regex_actions + 243; goto execFuncs;
	f133: _acts = _regex_actions + 245; goto execFuncs;
	f134: _acts = _regex_actions + 247; goto execFuncs;
	f135: _acts = _regex_actions + 249; goto execFuncs;
	f136: _acts = _regex_actions + 251; goto execFuncs;
	f137: _acts = _regex_actions + 253; goto execFuncs;
	f138: _acts = _regex_actions + 255; goto execFuncs;
	f139: _acts = _regex_actions + 257; goto execFuncs;
	f140: _acts = _regex_actions + 259; goto execFuncs;
	f141: _acts = _regex_actions + 261; goto execFuncs;
	f142: _acts = _regex_actions + 263; goto execFuncs;
	f144: _acts = _regex_actions + 265; goto execFuncs;
	f145: _acts = _regex_actions + 267; goto execFuncs;
	f146: _acts = _regex_actions + 269; goto execFuncs;
	f147: _acts = _regex_actions + 271; goto execFuncs;
	f149: _acts = _regex_actions + 273; goto execFuncs;
	f150: _acts = _regex_actions + 275; goto execFuncs;
	f151: _acts = _regex_actions + 277; goto execFuncs;
	f152: _acts = _regex_actions + 279; goto execFuncs;
	f153: _acts = _regex_actions + 281; goto execFuncs;
	f154: _acts = _regex_actions + 283; goto execFuncs;
	f155: _acts = _regex_actions + 285; goto execFuncs;
	f156: _acts = _regex_actions + 287; goto execFuncs;
	f157: _acts = _regex_actions + 289; goto execFuncs;
	f158: _acts = _regex_actions + 291; goto execFuncs;
	f159: _acts = _regex_actions + 293; goto execFuncs;
	f160: _acts = _regex_actions + 295; goto execFuncs;
	f161: _acts = _regex_actions + 297; goto execFuncs;
	f162: _acts = _regex_actions + 299; goto execFuncs;
	f164: _acts = _regex_actions + 301; goto execFuncs;
	f163: _acts = _regex_actions + 303; goto execFuncs;
	f165: _acts = _regex_actions + 305; goto execFuncs;
	f166: _acts = _regex_actions + 307; goto execFuncs;
	f167: _acts = _regex_actions + 309; goto execFuncs;
	f168: _acts = _regex_actions + 311; goto execFuncs;
	f365: _acts = _regex_actions + 313; goto execFuncs;
	f66: _acts = _regex_actions + 315; goto execFuncs;
	f328: _acts = _regex_actions + 317; goto execFuncs;
	f330: _acts = _regex_actions + 319; goto execFuncs;
	f335: _acts = _regex_actions + 321; goto execFuncs;
	f337: _acts = _regex_actions + 323; goto execFuncs;
	f344: _acts = _regex_actions + 325; goto execFuncs;
	f347: _acts = _regex_actions + 327; goto execFuncs;
	f348: _acts = _regex_actions + 329; goto execFuncs;
	f352: _acts = _regex_actions + 331; goto execFuncs;
	f360: _acts = _regex_actions + 333; goto execFuncs;
	f366: _acts = _regex_actions + 335; goto execFuncs;
	f336: _acts = _regex_actions + 337; goto execFuncs;
	f329: _acts = _regex_actions + 339; goto execFuncs;
	f79: _acts = _regex_actions + 341; goto execFuncs;
	f84: _acts = _regex_actions + 343; goto execFuncs;
	f116: _acts = _regex_actions + 345; goto execFuncs;
	f125: _acts = _regex_actions + 347; goto execFuncs;
	f128: _acts = _regex_actions + 349; goto execFuncs;
	f132: _acts = _regex_actions + 351; goto execFuncs;
	f143: _acts = _regex_actions + 353; goto execFuncs;
	f148: _acts = _regex_actions + 355; goto execFuncs;
	f100: _acts = _regex_actions + 357; goto execFuncs;
	f65: _acts = _regex_actions + 359; goto execFuncs;
	f371: _acts = _regex_actions + 361; goto execFuncs;
	f372: _acts = _regex_actions + 363; goto execFuncs;
	f373: _acts = _regex_actions + 365; goto execFuncs;
	f374: _acts = _regex_actions + 367; goto execFuncs;
	f375: _acts = _regex_actions + 369; goto execFuncs;
	f376: _acts = _regex_actions + 371; goto execFuncs;
	f377: _acts = _regex_actions + 373; goto execFuncs;
	f370: _acts = _regex_actions + 375; goto execFuncs;
	f179: _acts = _regex_actions + 377; goto execFuncs;
	f195: _acts = _regex_actions + 379; goto execFuncs;
	f181: _acts = _regex_actions + 381; goto execFuncs;
	f196: _acts = _regex_actions + 383; goto execFuncs;
	f182: _acts = _regex_actions + 385; goto execFuncs;
	f197: _acts = _regex_actions + 387; goto execFuncs;
	f183: _acts = _regex_actions + 389; goto execFuncs;
	f198: _acts = _regex_actions + 391; goto execFuncs;
	f184: _acts = _regex_actions + 393; goto execFuncs;
	f199: _acts = _regex_actions + 395; goto execFuncs;
	f185: _acts = _regex_actions + 397; goto execFuncs;
	f200: _acts = _regex_actions + 399; goto execFuncs;
	f186: _acts = _regex_actions + 401; goto execFuncs;
	f201: _acts = _regex_actions + 403; goto execFuncs;
	f187: _acts = _regex_actions + 405; goto execFuncs;
	f202: _acts = _regex_actions + 407; goto execFuncs;
	f188: _acts = _regex_actions + 409; goto execFuncs;
	f203: _acts = _regex_actions + 411; goto execFuncs;
	f189: _acts = _regex_actions + 413; goto execFuncs;
	f204: _acts = _regex_actions + 415; goto execFuncs;
	f190: _acts = _regex_actions + 417; goto execFuncs;
	f205: _acts = _regex_actions + 419; goto execFuncs;
	f191: _acts = _regex_actions + 421; goto execFuncs;
	f206: _acts = _regex_actions + 423; goto execFuncs;
	f192: _acts = _regex_actions + 425; goto execFuncs;
	f207: _acts = _regex_actions + 427; goto execFuncs;
	f193: _acts = _regex_actions + 429; goto execFuncs;
	f208: _acts = _regex_actions + 431; goto execFuncs;
	f194: _acts = _regex_actions + 433; goto execFuncs;
	f180: _acts = _regex_actions + 435; goto execFuncs;
	f391: _acts = _regex_actions + 437; goto execFuncs;
	f388: _acts = _regex_actions + 439; goto execFuncs;
	f396: _acts = _regex_actions + 441; goto execFuncs;
	f405: _acts = _regex_actions + 443; goto execFuncs;
	f402: _acts = _regex_actions + 445; goto execFuncs;
	f403: _acts = _regex_actions + 447; goto execFuncs;
	f399: _acts = _regex_actions + 449; goto execFuncs;
	f395: _acts = _regex_actions + 451; goto execFuncs;
	f398: _acts = _regex_actions + 453; goto execFuncs;
	f401: _acts = _regex_actions + 455; goto execFuncs;
	f389: _acts = _regex_actions + 457; goto execFuncs;
	f406: _acts = _regex_actions + 459; goto execFuncs;
	f393: _acts = _regex_actions + 461; goto execFuncs;
	f420: _acts = _regex_actions + 463; goto execFuncs;
	f419: _acts = _regex_actions + 465; goto execFuncs;
	f414: _acts = _regex_actions + 467; goto execFuncs;
	f413: _acts = _regex_actions + 469; goto execFuncs;
	f210: _acts = _regex_actions + 471; goto execFuncs;
	f385: _acts = _regex_actions + 473; goto execFuncs;
	f212: _acts = _regex_actions + 475; goto execFuncs;
	f416: _acts = _regex_actions + 477; goto execFuncs;
	f407: _acts = _regex_actions + 479; goto execFuncs;
	f394: _acts = _regex_actions + 481; goto execFuncs;
	f404: _acts = _regex_actions + 483; goto execFuncs;
	f392: _acts = _regex_actions + 485; goto execFuncs;
	f397: _acts = _regex_actions + 487; goto execFuncs;
	f387: _acts = _regex_actions + 489; goto execFuncs;
	f379: _acts = _regex_actions + 491; goto execFuncs;
	f390: _acts = _regex_actions + 493; goto execFuncs;
	f400: _acts = _regex_actions + 495; goto execFuncs;
	f386: _acts = _regex_actions + 497; goto execFuncs;
	f383: _acts = _regex_actions + 499; goto execFuncs;
	f427: _acts = _regex_actions + 501; goto execFuncs;
	f214: _acts = _regex_actions + 503; goto execFuncs;
	f215: _acts = _regex_actions + 505; goto execFuncs;
	f381: _acts = _regex_actions + 507; goto execFuncs;
	f378: _acts = _regex_actions + 509; goto execFuncs;
	f380: _acts = _regex_actions + 511; goto execFuncs;
	f412: _acts = _regex_actions + 513; goto execFuncs;
	f418: _acts = _regex_actions + 515; goto execFuncs;
	f408: _acts = _regex_actions + 517; goto execFuncs;
	f410: _acts = _regex_actions + 519; goto execFuncs;
	f417: _acts = _regex_actions + 521; goto execFuncs;
	f421: _acts = _regex_actions + 523; goto execFuncs;
	f425: _acts = _regex_actions + 525; goto execFuncs;
	f415: _acts = _regex_actions + 527; goto execFuncs;
	f426: _acts = _regex_actions + 529; goto execFuncs;
	f382: _acts = _regex_actions + 531; goto execFuncs;
	f209: _acts = _regex_actions + 533; goto execFuncs;
	f211: _acts = _regex_actions + 535; goto execFuncs;
	f213: _acts = _regex_actions + 537; goto execFuncs;
	f178: _acts = _regex_actions + 539; goto execFuncs;
	f430: _acts = _regex_actions + 541; goto execFuncs;
	f429: _acts = _regex_actions + 543; goto execFuncs;
	f433: _acts = _regex_actions + 545; goto execFuncs;
	f432: _acts = _regex_actions + 547; goto execFuncs;
	f428: _acts = _regex_actions + 549; goto execFuncs;
	f431: _acts = _regex_actions + 551; goto execFuncs;
	f437: _acts = _regex_actions + 553; goto execFuncs;
	f439: _acts = _regex_actions + 555; goto execFuncs;
	f217: _acts = _regex_actions + 557; goto execFuncs;
	f218: _acts = _regex_actions + 559; goto execFuncs;
	f435: _acts = _regex_actions + 561; goto execFuncs;
	f434: _acts = _regex_actions + 563; goto execFuncs;
	f438: _acts = _regex_actions + 565; goto execFuncs;
	f436: _acts = _regex_actions + 567; goto execFuncs;
	f216: _acts = _regex_actions + 569; goto execFuncs;
	f443: _acts = _regex_actions + 571; goto execFuncs;
	f445: _acts = _regex_actions + 573; goto execFuncs;
	f220: _acts = _regex_actions + 575; goto execFuncs;
	f221: _acts = _regex_actions + 577; goto execFuncs;
	f441: _acts = _regex_actions + 579; goto execFuncs;
	f440: _acts = _regex_actions + 581; goto execFuncs;
	f444: _acts = _regex_actions + 583; goto execFuncs;
	f442: _acts = _regex_actions + 585; goto execFuncs;
	f219: _acts = _regex_actions + 587; goto execFuncs;
	f447: _acts = _regex_actions + 589; goto execFuncs;
	f446: _acts = _regex_actions + 591; goto execFuncs;
	f449: _acts = _regex_actions + 593; goto execFuncs;
	f448: _acts = _regex_actions + 595; goto execFuncs;
	f227: _acts = _regex_actions + 597; goto execFuncs;
	f231: _acts = _regex_actions + 599; goto execFuncs;
	f36: _acts = _regex_actions + 601; goto execFuncs;
	f35: _acts = _regex_actions + 603; goto execFuncs;
	f266: _acts = _regex_actions + 605; goto execFuncs;
	f261: _acts = _regex_actions + 607; goto execFuncs;
	f228: _acts = _regex_actions + 609; goto execFuncs;
	f259: _acts = _regex_actions + 611; goto execFuncs;
	f244: _acts = _regex_actions + 613; goto execFuncs;
	f243: _acts = _regex_actions + 615; goto execFuncs;
	f247: _acts = _regex_actions + 617; goto execFuncs;
	f246: _acts = _regex_actions + 619; goto execFuncs;
	f250: _acts = _regex_actions + 621; goto execFuncs;
	f249: _acts = _regex_actions + 623; goto execFuncs;
	f323: _acts = _regex_actions + 625; goto execFuncs;
	f322: _acts = _regex_actions + 627; goto execFuncs;
	f233: _acts = _regex_actions + 629; goto execFuncs;
	f1: _acts = _regex_actions + 631; goto execFuncs;
	f223: _acts = _regex_actions + 633; goto execFuncs;
	f229: _acts = _regex_actions + 635; goto execFuncs;
	f226: _acts = _regex_actions + 637; goto execFuncs;
	f257: _acts = _regex_actions + 639; goto execFuncs;
	f272: _acts = _regex_actions + 641; goto execFuncs;
	f286: _acts = _regex_actions + 643; goto execFuncs;
	f274: _acts = _regex_actions + 645; goto execFuncs;
	f258: _acts = _regex_actions + 647; goto execFuncs;
	f283: _acts = _regex_actions + 649; goto execFuncs;
	f280: _acts = _regex_actions + 651; goto execFuncs;
	f281: _acts = _regex_actions + 653; goto execFuncs;
	f277: _acts = _regex_actions + 655; goto execFuncs;
	f273: _acts = _regex_actions + 657; goto execFuncs;
	f276: _acts = _regex_actions + 659; goto execFuncs;
	f42: _acts = _regex_actions + 661; goto execFuncs;
	f41: _acts = _regex_actions + 663; goto execFuncs;
	f43: _acts = _regex_actions + 665; goto execFuncs;
	f47: _acts = _regex_actions + 667; goto execFuncs;
	f46: _acts = _regex_actions + 669; goto execFuncs;
	f45: _acts = _regex_actions + 671; goto execFuncs;
	f32: _acts = _regex_actions + 673; goto execFuncs;
	f38: _acts = _regex_actions + 675; goto execFuncs;
	f49: _acts = _regex_actions + 677; goto execFuncs;
	f51: _acts = _regex_actions + 679; goto execFuncs;
	f300: _acts = _regex_actions + 681; goto execFuncs;
	f265: _acts = _regex_actions + 683; goto execFuncs;
	f285: _acts = _regex_actions + 685; goto execFuncs;
	f270: _acts = _regex_actions + 687; goto execFuncs;
	f282: _acts = _regex_actions + 689; goto execFuncs;
	f268: _acts = _regex_actions + 691; goto execFuncs;
	f275: _acts = _regex_actions + 693; goto execFuncs;
	f260: _acts = _regex_actions + 695; goto execFuncs;
	f279: _acts = _regex_actions + 697; goto execFuncs;
	f263: _acts = _regex_actions + 699; goto execFuncs;
	f284: _acts = _regex_actions + 701; goto execFuncs;
	f269: _acts = _regex_actions + 703; goto execFuncs;
	f311: _acts = _regex_actions + 705; goto execFuncs;
	f310: _acts = _regex_actions + 707; goto execFuncs;
	f298: _acts = _regex_actions + 709; goto execFuncs;
	f297: _acts = _regex_actions + 711; goto execFuncs;
	f267: _acts = _regex_actions + 713; goto execFuncs;
	f264: _acts = _regex_actions + 715; goto execFuncs;
	f262: _acts = _regex_actions + 717; goto execFuncs;
	f271: _acts = _regex_actions + 719; goto execFuncs;
	f253: _acts = _regex_actions + 721; goto execFuncs;
	f4: _acts = _regex_actions + 723; goto execFuncs;
	f28: _acts = _regex_actions + 725; goto execFuncs;
	f29: _acts = _regex_actions + 727; goto execFuncs;
	f10: _acts = _regex_actions + 729; goto execFuncs;
	f3: _acts = _regex_actions + 731; goto execFuncs;
	f240: _acts = _regex_actions + 733; goto execFuncs;
	f239: _acts = _regex_actions + 735; goto execFuncs;
	f13: _acts = _regex_actions + 737; goto execFuncs;
	f241: _acts = _regex_actions + 739; goto execFuncs;
	f11: _acts = _regex_actions + 741; goto execFuncs;
	f16: _acts = _regex_actions + 743; goto execFuncs;
	f27: _acts = _regex_actions + 745; goto execFuncs;
	f15: _acts = _regex_actions + 747; goto execFuncs;
	f23: _acts = _regex_actions + 749; goto execFuncs;
	f22: _acts = _regex_actions + 751; goto execFuncs;
	f25: _acts = _regex_actions + 753; goto execFuncs;
	f24: _acts = _regex_actions + 755; goto execFuncs;
	f26: _acts = _regex_actions + 757; goto execFuncs;
	f20: _acts = _regex_actions + 759; goto execFuncs;
	f19: _acts = _regex_actions + 761; goto execFuncs;
	f31: _acts = _regex_actions + 763; goto execFuncs;
	f2: _acts = _regex_actions + 765; goto execFuncs;
	f325: _acts = _regex_actions + 767; goto execFuncs;
	f58: _acts = _regex_actions + 769; goto execFuncs;
	f59: _acts = _regex_actions + 771; goto execFuncs;
	f232: _acts = _regex_actions + 773; goto execFuncs;
	f225: _acts = _regex_actions + 775; goto execFuncs;
	f224: _acts = _regex_actions + 777; goto execFuncs;
	f234: _acts = _regex_actions + 779; goto execFuncs;
	f251: _acts = _regex_actions + 781; goto execFuncs;
	f242: _acts = _regex_actions + 783; goto execFuncs;
	f245: _acts = _regex_actions + 785; goto execFuncs;
	f248: _acts = _regex_actions + 787; goto execFuncs;
	f321: _acts = _regex_actions + 789; goto execFuncs;
	f287: _acts = _regex_actions + 791; goto execFuncs;
	f293: _acts = _regex_actions + 793; goto execFuncs;
	f290: _acts = _regex_actions + 795; goto execFuncs;
	f305: _acts = _regex_actions + 797; goto execFuncs;
	f303: _acts = _regex_actions + 799; goto execFuncs;
	f301: _acts = _regex_actions + 801; goto execFuncs;
	f308: _acts = _regex_actions + 803; goto execFuncs;
	f312: _acts = _regex_actions + 805; goto execFuncs;
	f319: _acts = _regex_actions + 807; goto execFuncs;
	f299: _acts = _regex_actions + 809; goto execFuncs;
	f296: _acts = _regex_actions + 811; goto execFuncs;
	f309: _acts = _regex_actions + 813; goto execFuncs;
	f307: _acts = _regex_actions + 815; goto execFuncs;
	f252: _acts = _regex_actions + 817; goto execFuncs;
	f236: _acts = _regex_actions + 819; goto execFuncs;
	f235: _acts = _regex_actions + 821; goto execFuncs;
	f324: _acts = _regex_actions + 823; goto execFuncs;
	f320: _acts = _regex_actions + 825; goto execFuncs;
	f0: _acts = _regex_actions + 827; goto execFuncs;
	f34: _acts = _regex_actions + 829; goto execFuncs;
	f37: _acts = _regex_actions + 831; goto execFuncs;
	f48: _acts = _regex_actions + 833; goto execFuncs;
	f50: _acts = _regex_actions + 835; goto execFuncs;
	f44: _acts = _regex_actions + 837; goto execFuncs;
	f18: _acts = _regex_actions + 839; goto execFuncs;
	f14: _acts = _regex_actions + 841; goto execFuncs;
	f57: _acts = _regex_actions + 843; goto execFuncs;
	f52: _acts = _regex_actions + 845; goto execFuncs;
	f295: _acts = _regex_actions + 847; goto execFuncs;
	f302: _acts = _regex_actions + 849; goto execFuncs;
	f255: _acts = _regex_actions + 852; goto execFuncs;
	f238: _acts = _regex_actions + 855; goto execFuncs;
	f291: _acts = _regex_actions + 858; goto execFuncs;
	f409: _acts = _regex_actions + 861; goto execFuncs;
	f411: _acts = _regex_actions + 864; goto execFuncs;
	f289: _acts = _regex_actions + 867; goto execFuncs;
	f21: _acts = _regex_actions + 870; goto execFuncs;
	f306: _acts = _regex_actions + 873; goto execFuncs;
	f304: _acts = _regex_actions + 876; goto execFuncs;
	f422: _acts = _regex_actions + 879; goto execFuncs;
	f316: _acts = _regex_actions + 882; goto execFuncs;
	f424: _acts = _regex_actions + 885; goto execFuncs;
	f318: _acts = _regex_actions + 888; goto execFuncs;
	f423: _acts = _regex_actions + 891; goto execFuncs;
	f317: _acts = _regex_actions + 894; goto execFuncs;
	f7: _acts = _regex_actions + 897; goto execFuncs;
	f9: _acts = _regex_actions + 900; goto execFuncs;
	f6: _acts = _regex_actions + 903; goto execFuncs;
	f278: _acts = _regex_actions + 906; goto execFuncs;
	f230: _acts = _regex_actions + 909; goto execFuncs;
	f8: _acts = _regex_actions + 912; goto execFuncs;
	f256: _acts = _regex_actions + 915; goto execFuncs;
	f292: _acts = _regex_actions + 919; goto execFuncs;
	f12: _acts = _regex_actions + 923; goto execFuncs;
	f294: _acts = _regex_actions + 927; goto execFuncs;

execFuncs:
	_nacts = *_acts++;
	while ( _nacts-- > 0 ) {
		switch ( *_acts++ ) {
	case 0:
#line 286 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ label.clear();}
	break;
	case 1:
#line 287 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ label.push_back((*p));}
	break;
	case 2:
#line 288 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ octAccumulator = 0;}
	break;
	case 3:
#line 289 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ accumulator = 0;}
	break;
	case 4:
#line 290 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        octAccumulator = 0;
        pushOct(&octAccumulator, (*p));
    }
	break;
	case 5:
#line 294 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        accumulator = 0;
        pushDec(&accumulator, (*p));
    }
	break;
	case 6:
#line 298 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ repeatN = 0; repeatM = 0; }
	break;
	case 7:
#line 299 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ pushDec(&repeatN, (*p)); }
	break;
	case 8:
#line 300 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ pushDec(&repeatM, (*p)); }
	break;
	case 9:
#line 301 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ pushOct(&octAccumulator, (*p)); }
	break;
	case 10:
#line 302 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ pushDec(&accumulator, (*p)); }
	break;
	case 11:
#line 303 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        accumulator *= 16;
        accumulator += (*p) - '0';
    }
	break;
	case 12:
#line 307 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        accumulator *= 16;
        accumulator += 10 + (*p) - 'a';
    }
	break;
	case 13:
#line 311 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        accumulator *= 16;
        accumulator += 10 + (*p) - 'A';
    }
	break;
	case 14:
#line 431 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        newMode = mode;
    }
	break;
	case 15:
#line 438 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        switch ((*p)) {
            case 'i':
                newMode.caseless = true;
                break;
            case 'm':
                newMode.multiline = true;
                break;
            case 's':
                newMode.dotall = true;
                break;
            case 'x':
                newMode.ignore_space = true;
                break;
            default:
                assert(0); // this action only called for [imsx]
                break;
        }
    }
	break;
	case 16:
#line 457 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{
        switch ((*p)) {
            case 'i':
                newMode.caseless = false;
                break;
            case 'm':
                newMode.multiline = false;
                break;
            case 's':
                newMode.dotall = false;
                break;
            case 'x':
                newMode.ignore_space = false;
                break;
            default:
                assert(0); // this action only called for [imsx]
                break;
        }
    }
	break;
	case 17:
#line 511 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{repeatM = repeatN;}
	break;
	case 18:
#line 511 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{repeatM = ComponentRepeat::NoLimit;}
	break;
	case 19:
#line 723 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ negated = !negated; }
	break;
	case 20:
#line 724 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ p--; {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 790;goto _again;}} }
	break;
	case 21:
#line 725 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ if (!inCharClass) { // not inside [..]
                                 currentCls->finalize();
                                 currentSeq->addComponent(std::move(currentCls));
                             }
                             {cs = stack[--top];goto _again;} 
                           }
	break;
	case 22:
#line 731 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ throw LocatedParseError("Malformed property"); }
	break;
	case 25:
#line 1 "NONE"
	{te = p+1;}
	break;
	case 26:
#line 551 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            throw LocatedParseError("(*UTF8) must be at start of "
                                    "expression, encountered");
        }}
	break;
	case 27:
#line 555 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            throw LocatedParseError("(*UTF) must be at start of "
                                    "expression, encountered");
        }}
	break;
	case 28:
#line 559 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            throw LocatedParseError("(*UCP) must be at start of "
                                    "expression, encountered");
        }}
	break;
	case 29:
#line 565 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            ParseMode temp_mode;
            assert(ts - 2 >= ptr); // parser needs the '(*' at the start too.
            read_control_verbs(ts - 2, te, (ts - 2 - ptr), temp_mode);
            assert(0); // Should have thrown a parse error.
            throw LocatedParseError("Unknown control verb");
        }}
	break;
	case 30:
#line 572 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            throw LocatedParseError("Unknown control verb");
        }}
	break;
	case 31:
#line 572 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
            throw LocatedParseError("Unknown control verb");
        }}
	break;
	case 32:
#line 572 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
            throw LocatedParseError("Unknown control verb");
        }}
	break;
	case 33:
#line 582 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_CC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 34:
#line 583 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_CF, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 35:
#line 584 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_CN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 36:
#line 586 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_CS, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 37:
#line 588 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_LL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 38:
#line 589 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_LM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 39:
#line 590 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_LO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 40:
#line 591 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_LT, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 41:
#line 592 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_LU, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 42:
#line 593 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_L_AND, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 43:
#line 595 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_MC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 44:
#line 597 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_MN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 45:
#line 599 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_ND, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 46:
#line 600 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_NL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 47:
#line 601 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_NO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 48:
#line 603 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 49:
#line 604 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PD, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 50:
#line 605 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 51:
#line 606 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PF, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 52:
#line 607 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 53:
#line 608 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 54:
#line 609 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_PS, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 55:
#line 611 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_SC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 56:
#line 612 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_SK, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 57:
#line 613 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_SM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 58:
#line 614 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_SO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 59:
#line 616 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_ZL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 60:
#line 617 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_ZP, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 61:
#line 618 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_ZS, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 62:
#line 619 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_XAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 63:
#line 620 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_XPS, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 64:
#line 621 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_XSP, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 65:
#line 622 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_XWD, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 66:
#line 623 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_ARABIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 67:
#line 624 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_ARMENIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 68:
#line 625 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_AVESTAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 69:
#line 626 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BALINESE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 70:
#line 627 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BAMUM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 71:
#line 628 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BATAK, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 72:
#line 629 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BENGALI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 73:
#line 630 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BOPOMOFO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 74:
#line 631 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BRAHMI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 75:
#line 632 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BRAILLE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 76:
#line 633 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BUGINESE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 77:
#line 634 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_BUHID, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 78:
#line 635 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CANADIAN_ABORIGINAL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 79:
#line 636 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CARIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 80:
#line 637 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CHAM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 81:
#line 638 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CHEROKEE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 82:
#line 639 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_COMMON, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 83:
#line 640 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_COPTIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 84:
#line 641 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CUNEIFORM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 85:
#line 642 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CYPRIOT, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 86:
#line 643 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_CYRILLIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 87:
#line 644 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_DESERET, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 88:
#line 645 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_DEVANAGARI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 89:
#line 646 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_EGYPTIAN_HIEROGLYPHS, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 90:
#line 647 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_ETHIOPIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 91:
#line 648 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GEORGIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 92:
#line 649 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GLAGOLITIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 93:
#line 650 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GOTHIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 94:
#line 651 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GREEK, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 95:
#line 652 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GUJARATI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 96:
#line 653 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_GURMUKHI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 97:
#line 655 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_HANGUL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 98:
#line 656 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_HANUNOO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 99:
#line 657 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_HEBREW, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 100:
#line 658 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_HIRAGANA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 101:
#line 659 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_IMPERIAL_ARAMAIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 102:
#line 660 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_INHERITED, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 103:
#line 661 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_INSCRIPTIONAL_PAHLAVI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 104:
#line 662 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_INSCRIPTIONAL_PARTHIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 105:
#line 663 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_JAVANESE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 106:
#line 664 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KAITHI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 107:
#line 665 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KANNADA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 108:
#line 666 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KATAKANA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 109:
#line 667 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KAYAH_LI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 110:
#line 668 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KHAROSHTHI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 111:
#line 669 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_KHMER, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 112:
#line 670 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LAO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 113:
#line 671 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LATIN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 114:
#line 672 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LEPCHA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 115:
#line 673 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LIMBU, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 116:
#line 674 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LINEAR_B, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 117:
#line 675 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LISU, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 118:
#line 676 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LYCIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 119:
#line 677 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_LYDIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 120:
#line 678 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_MALAYALAM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 121:
#line 679 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_MANDAIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 122:
#line 680 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_MEETEI_MAYEK, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 123:
#line 681 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_MONGOLIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 124:
#line 682 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_MYANMAR, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 125:
#line 683 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_NEW_TAI_LUE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 126:
#line 684 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_NKO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 127:
#line 685 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OGHAM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 128:
#line 686 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OL_CHIKI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 129:
#line 687 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OLD_ITALIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 130:
#line 688 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OLD_PERSIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 131:
#line 689 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OLD_SOUTH_ARABIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 132:
#line 690 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OLD_TURKIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 133:
#line 691 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_ORIYA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 134:
#line 692 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_OSMANYA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 135:
#line 693 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_PHAGS_PA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 136:
#line 694 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_PHOENICIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 137:
#line 695 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_REJANG, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 138:
#line 696 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_RUNIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 139:
#line 697 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SAMARITAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 140:
#line 698 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SAURASHTRA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 141:
#line 699 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SHAVIAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 142:
#line 700 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SINHALA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 143:
#line 701 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SUNDANESE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 144:
#line 702 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SYLOTI_NAGRI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 145:
#line 703 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_SYRIAC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 146:
#line 704 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAGALOG, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 147:
#line 705 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAGBANWA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 148:
#line 706 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAI_LE, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 149:
#line 707 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAI_THAM, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 150:
#line 708 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAI_VIET, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 151:
#line 709 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TAMIL, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 152:
#line 710 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TELUGU, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 153:
#line 711 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_THAANA, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 154:
#line 712 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_THAI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 155:
#line 713 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TIBETAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 156:
#line 714 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_TIFINAGH, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 157:
#line 715 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_UGARITIC, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 158:
#line 716 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_VAI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 159:
#line 717 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_SCRIPT_YI, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 160:
#line 718 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ currentCls->add(CLASS_UCP_ANY, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 161:
#line 719 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ throw LocatedParseError("Unknown property"); }}
	break;
	case 162:
#line 581 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_C, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 163:
#line 585 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_CO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 164:
#line 587 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_L, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 165:
#line 594 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_M, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 166:
#line 596 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_ME, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 167:
#line 598 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_N, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 168:
#line 602 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_P, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 169:
#line 610 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_S, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 170:
#line 615 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_UCP_Z, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 171:
#line 654 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ currentCls->add(CLASS_SCRIPT_HAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 172:
#line 719 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ throw LocatedParseError("Unknown property"); }}
	break;
	case 173:
#line 581 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_C, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 174:
#line 585 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_CO, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 175:
#line 587 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_L, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 176:
#line 594 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_M, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 177:
#line 596 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_ME, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 178:
#line 598 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_N, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 179:
#line 602 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_P, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 180:
#line 610 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_UCP_S, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 181:
#line 654 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ currentCls->add(CLASS_SCRIPT_HAN, negated); {cs = stack[--top];goto _again;} }}
	break;
	case 182:
#line 719 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{ throw LocatedParseError("Unknown property"); }}
	break;
	case 183:
#line 734 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_C, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 184:
#line 742 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_L, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 185:
#line 750 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_M, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 186:
#line 758 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            currentCls->add(CLASS_UCP_N, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;}
        }}
	break;
	case 187:
#line 766 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_P, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 188:
#line 774 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_S, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 189:
#line 782 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
            currentCls->add(CLASS_UCP_Z, negated); 
            if (!inCharClass) {
                currentCls->finalize();
                currentSeq->addComponent(std::move(currentCls));
            }
            {cs = stack[--top];goto _again;} 
        }}
	break;
	case 190:
#line 791 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ throw LocatedParseError("Unknown property"); }}
	break;
	case 191:
#line 797 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("Unsupported POSIX collating "
                                          "element");
              }}
	break;
	case 192:
#line 804 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ALNUM, false);
              }}
	break;
	case 193:
#line 807 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ALNUM, true);
              }}
	break;
	case 194:
#line 810 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ALPHA, false);
              }}
	break;
	case 195:
#line 813 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ALPHA, true);
              }}
	break;
	case 196:
#line 816 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ASCII, false);
              }}
	break;
	case 197:
#line 819 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_ASCII, true);
              }}
	break;
	case 198:
#line 822 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_BLANK, false);
              }}
	break;
	case 199:
#line 825 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_BLANK, true);
              }}
	break;
	case 200:
#line 828 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_CNTRL, false);
              }}
	break;
	case 201:
#line 831 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_CNTRL, true);
              }}
	break;
	case 202:
#line 834 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_DIGIT, false);
              }}
	break;
	case 203:
#line 837 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_DIGIT, true);
              }}
	break;
	case 204:
#line 840 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_GRAPH, false);
              }}
	break;
	case 205:
#line 843 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_GRAPH, true);
              }}
	break;
	case 206:
#line 846 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_LOWER, false);
              }}
	break;
	case 207:
#line 849 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_LOWER, true);
              }}
	break;
	case 208:
#line 852 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_PRINT, false);
              }}
	break;
	case 209:
#line 855 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_PRINT, true);
              }}
	break;
	case 210:
#line 858 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_PUNCT, false);
              }}
	break;
	case 211:
#line 861 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_PUNCT, true);
              }}
	break;
	case 212:
#line 865 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_SPACE, false);
              }}
	break;
	case 213:
#line 868 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_SPACE, true);
              }}
	break;
	case 214:
#line 871 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_UPPER, false);
              }}
	break;
	case 215:
#line 874 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_UPPER, true);
              }}
	break;
	case 216:
#line 877 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_WORD, false);
              }}
	break;
	case 217:
#line 880 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_WORD, true);
              }}
	break;
	case 218:
#line 883 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_XDIGIT, false);
              }}
	break;
	case 219:
#line 886 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_XDIGIT, true);
              }}
	break;
	case 220:
#line 891 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("Invalid POSIX named class");
              }}
	break;
	case 221:
#line 894 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 843;goto _again;}}
              }}
	break;
	case 222:
#line 897 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ /*noop*/}}
	break;
	case 223:
#line 899 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x08');
              }}
	break;
	case 224:
#line 903 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x09');
              }}
	break;
	case 225:
#line 907 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x0a');
              }}
	break;
	case 226:
#line 911 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x0d');
              }}
	break;
	case 227:
#line 915 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x0c');
              }}
	break;
	case 228:
#line 919 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x07');
              }}
	break;
	case 229:
#line 923 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add('\x1b');
              }}
	break;
	case 230:
#line 927 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_HORZ, false);
              }}
	break;
	case 231:
#line 931 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_HORZ, true);
              }}
	break;
	case 232:
#line 935 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_VERT, false);
              }}
	break;
	case 233:
#line 939 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_VERT, true);
              }}
	break;
	case 234:
#line 943 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  negated = false;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 559;goto _again;}}
              }}
	break;
	case 235:
#line 949 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  negated = false;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 818;goto _again;}}
              }}
	break;
	case 236:
#line 955 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  negated = true;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 559;goto _again;}}
              }}
	break;
	case 237:
#line 961 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  negated = true;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 818;goto _again;}}
              }}
	break;
	case 238:
#line 971 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(octAccumulator);
              }}
	break;
	case 239:
#line 974 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(octAccumulator);
              }}
	break;
	case 240:
#line 978 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  string oct(ts + 3, te - ts - 4);
                  unsigned long val;
                  try {
                      val = stoul(oct, nullptr, 8);
                  } catch (const std::out_of_range &) {
                      val = MAX_UNICODE + 1;
                  }
                  if ((!mode.utf8 && val > 255) || val > MAX_UNICODE) {
                      throw LocatedParseError("Value in \\o{...} sequence is too large");
                  }
                  currentCls->add((unichar)val);
              }}
	break;
	case 241:
#line 998 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(accumulator);
              }}
	break;
	case 242:
#line 1002 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  // whatever we found here
                  currentCls->add(*(ts + 1));

              }}
	break;
	case 243:
#line 1008 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  string hex(ts + 3, te - ts - 4);
                  unsigned long val;
                  try {
                      val = stoul(hex, nullptr, 16);
                  } catch (const std::out_of_range &) {
                      val = MAX_UNICODE + 1;
                  }
                  if (val > MAX_UNICODE) {
                      throw LocatedParseError("Value in \\x{...} sequence is too large");
                  }
                  currentCls->add((unichar)val);
              }}
	break;
	case 244:
#line 1026 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (te - ts < 3) {
                      assert(te - ts == 2);
                      throw LocatedParseError(SLASH_C_ERROR);
                  } else {
                      assert(te - ts == 3);
                      currentCls->add(decodeCtrl(ts[2]));
                  }
              }}
	break;
	case 245:
#line 1036 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_WORD, false);
              }}
	break;
	case 246:
#line 1040 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_WORD, true);
              }}
	break;
	case 247:
#line 1044 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_SPACE, false);
              }}
	break;
	case 248:
#line 1048 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_SPACE, true);
              }}
	break;
	case 249:
#line 1052 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_DIGIT, false);
              }}
	break;
	case 250:
#line 1056 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(CLASS_DIGIT, true);
              }}
	break;
	case 251:
#line 1059 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->addDash();
              }}
	break;
	case 252:
#line 277 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        ostringstream str;
        str << "'\\" << *(ts + 1) << "' at index " << ts - ptr
            << " not supported in a character class.";
        throw ParseError(str.str());
    }}
	break;
	case 253:
#line 277 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        ostringstream str;
        str << "'\\" << *(ts + 1) << "' at index " << ts - ptr
            << " not supported in a character class.";
        throw ParseError(str.str());
    }}
	break;
	case 254:
#line 277 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        ostringstream str;
        str << "'\\" << *(ts + 1) << "' at index " << ts - ptr
            << " not supported in a character class.";
        throw ParseError(str.str());
    }}
	break;
	case 255:
#line 1076 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  // add the literal char
                  currentCls->add(*(ts + 1));
              }}
	break;
	case 256:
#line 1082 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint2c(ts));
              }}
	break;
	case 257:
#line 1087 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint3c(ts));
              }}
	break;
	case 258:
#line 1092 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint4c(ts));
              }}
	break;
	case 259:
#line 1097 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 260:
#line 1103 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add((u8)*ts);
              }}
	break;
	case 261:
#line 1107 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->finalize();
                  currentSeq->addComponent(std::move(currentCls));
                  inCharClass = false;
                  {cs = 746;goto _again;}
              }}
	break;
	case 262:
#line 967 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ throw LocatedParseError("Malformed property"); }}
	break;
	case 263:
#line 968 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ throw LocatedParseError("Malformed property"); }}
	break;
	case 264:
#line 971 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  currentCls->add(octAccumulator);
              }}
	break;
	case 265:
#line 974 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  currentCls->add(octAccumulator);
              }}
	break;
	case 266:
#line 993 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Value in \\o{...} sequence is non-octal or missing braces");
              }}
	break;
	case 267:
#line 998 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  currentCls->add(accumulator);
              }}
	break;
	case 268:
#line 1022 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Value in \\x{...} sequence is non-hex or missing }");
              }}
	break;
	case 269:
#line 1026 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (te - ts < 3) {
                      assert(te - ts == 2);
                      throw LocatedParseError(SLASH_C_ERROR);
                  } else {
                      assert(te - ts == 3);
                      currentCls->add(decodeCtrl(ts[2]));
                  }
              }}
	break;
	case 270:
#line 1097 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 271:
#line 1103 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  currentCls->add((u8)*ts);
              }}
	break;
	case 272:
#line 993 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Value in \\o{...} sequence is non-octal or missing braces");
              }}
	break;
	case 273:
#line 1022 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Value in \\x{...} sequence is non-hex or missing }");
              }}
	break;
	case 274:
#line 1097 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 275:
#line 1103 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  currentCls->add((u8)*ts);
              }}
	break;
	case 276:
#line 1121 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            if (currentCls->isNegated()) {
                // Already seen a caret; the second one is not a meta-character.
                inCharClassEarly = false;
                p--; {cs = 819;goto _again;}
            } else {
                currentCls->negate();
                // Note: we cannot switch off inCharClassEarly here, as /[^]]/
                // needs to use the right square bracket path below.
            }
        }}
	break;
	case 277:
#line 1134 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            currentCls->add(']');
            inCharClassEarly = false;
        }}
	break;
	case 278:
#line 1139 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 843;goto _again;}} }}
	break;
	case 279:
#line 1140 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ /*noop*/}}
	break;
	case 280:
#line 1143 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
            inCharClassEarly = false;
            p--;
            {cs = 819;goto _again;}
        }}
	break;
	case 281:
#line 1143 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
            inCharClassEarly = false;
            p--;
            {cs = 819;goto _again;}
        }}
	break;
	case 282:
#line 1155 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  {cs = 746;goto _again;}
              }}
	break;
	case 283:
#line 1160 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint2c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 284:
#line 1169 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint3c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 285:
#line 1178 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint4c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 286:
#line 1187 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 287:
#line 1193 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, *ts, mode);
              }}
	break;
	case 288:
#line 1187 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 289:
#line 1193 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  addLiteral(currentSeq, *ts, mode);
              }}
	break;
	case 290:
#line 1187 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 291:
#line 1203 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  {cs = stack[--top];goto _again;}
              }}
	break;
	case 292:
#line 1208 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint2c(ts));
                  inCharClassEarly = false;
              }}
	break;
	case 293:
#line 1214 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint3c(ts));
                  inCharClassEarly = false;
              }}
	break;
	case 294:
#line 1220 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  currentCls->add(readUtf8CodePoint4c(ts));
                  inCharClassEarly = false;
              }}
	break;
	case 295:
#line 1226 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 296:
#line 1232 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentCls->add(*ts);
                  inCharClassEarly = false;
              }}
	break;
	case 297:
#line 1226 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 298:
#line 1232 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  currentCls->add(*ts);
                  inCharClassEarly = false;
              }}
	break;
	case 299:
#line 1226 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 300:
#line 1244 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ inComment = false; {cs = 746;goto _again;} }}
	break;
	case 301:
#line 1248 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;}
	break;
	case 302:
#line 1256 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ inComment = false; {cs = 746;goto _again;} }}
	break;
	case 303:
#line 1260 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;}
	break;
	case 304:
#line 1492 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{act = 288;}
	break;
	case 305:
#line 1509 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{act = 290;}
	break;
	case 306:
#line 1738 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{act = 330;}
	break;
	case 307:
#line 363 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        if (sequences.empty()) {
            throw LocatedParseError("Unmatched parentheses");
        }
        currentSeq->finalize();
        POP_SEQUENCE;
    }}
	break;
	case 308:
#line 1275 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentSeq->addAlternation();
              }}
	break;
	case 309:
#line 1280 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("POSIX named classes are only "
                                          "supported inside a class");
              }}
	break;
	case 310:
#line 1287 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("Unsupported POSIX collating "
                                          "element");
              }}
	break;
	case 311:
#line 1294 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  {cs = 838;goto _again;}
              }}
	break;
	case 312:
#line 1298 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ /* noop */ }}
	break;
	case 313:
#line 1300 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentSeq->addComponent(generateComponent(CLASS_ANY, false, mode));
              }}
	break;
	case 314:
#line 1304 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (mode.utf8) {
                      throw LocatedParseError("\\C is unsupported in UTF8");
                  }
                  currentSeq->addComponent(std::make_unique<ComponentByte>());
              }}
	break;
	case 315:
#line 1318 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(0, ComponentRepeat::NoLimit,
                                        ComponentRepeat::REPEAT_NONGREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 316:
#line 1325 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(0, ComponentRepeat::NoLimit,
                                        ComponentRepeat::REPEAT_POSSESSIVE)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 317:
#line 1339 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(1, ComponentRepeat::NoLimit,
                                        ComponentRepeat::REPEAT_NONGREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 318:
#line 1346 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(1, ComponentRepeat::NoLimit,
                                        ComponentRepeat::REPEAT_POSSESSIVE)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 319:
#line 1360 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(
                           0, 1, ComponentRepeat::REPEAT_NONGREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 320:
#line 1367 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (!currentSeq->addRepeat(
                           0, 1, ComponentRepeat::REPEAT_POSSESSIVE)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 321:
#line 1384 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (repeatN > repeatM || repeatM == 0) {
                      throwInvalidRepeat();
                  } else if (!currentSeq->addRepeat(
                                  repeatN, repeatM,
                                  ComponentRepeat::REPEAT_NONGREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 322:
#line 1394 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (repeatN > repeatM || repeatM == 0) {
                      throwInvalidRepeat();
                  } else if (!currentSeq->addRepeat(
                                  repeatN, repeatM,
                                  ComponentRepeat::REPEAT_POSSESSIVE)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 323:
#line 323 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        inComment = true;
        {cs = 849;goto _again;}
    }}
	break;
	case 324:
#line 1411 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ p--; {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 787;goto _again;}} }}
	break;
	case 325:
#line 1415 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ assert(0); {p++; goto _out; } }}
	break;
	case 326:
#line 1422 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto bound = mode.multiline ? ComponentBoundary::BEGIN_LINE
                                              : ComponentBoundary::BEGIN_STRING;
                  currentSeq->addComponent(std::make_unique<ComponentBoundary>(bound));
              }}
	break;
	case 327:
#line 1429 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto bound = mode.multiline ? ComponentBoundary::END_LINE
                                              : ComponentBoundary::END_STRING_OPTIONAL_LF;
                  currentSeq->addComponent(std::make_unique<ComponentBoundary>(bound));
              }}
	break;
	case 328:
#line 1435 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto bound = ComponentBoundary::BEGIN_STRING;
                  currentSeq->addComponent(std::make_unique<ComponentBoundary>(bound));
              }}
	break;
	case 329:
#line 1440 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto bound = ComponentBoundary::END_STRING_OPTIONAL_LF;
                  currentSeq->addComponent(std::make_unique<ComponentBoundary>(bound));
              }}
	break;
	case 330:
#line 1445 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto bound = ComponentBoundary::END_STRING;
                  currentSeq->addComponent(std::make_unique<ComponentBoundary>(bound));
              }}
	break;
	case 331:
#line 1450 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentSeq->addComponent(
                      std::make_unique<ComponentWordBoundary>(ts - ptr, false, mode));
              }}
	break;
	case 332:
#line 1455 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentSeq->addComponent(
                      std::make_unique<ComponentWordBoundary>(ts - ptr, true, mode));
              }}
	break;
	case 333:
#line 1465 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x09', mode);
              }}
	break;
	case 334:
#line 1469 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x0a', mode);
              }}
	break;
	case 335:
#line 1473 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x0d', mode);
              }}
	break;
	case 336:
#line 1477 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x0c', mode);
              }}
	break;
	case 337:
#line 1481 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x07', mode);
              }}
	break;
	case 338:
#line 1485 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, '\x1b', mode);
              }}
	break;
	case 339:
#line 1489 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, octAccumulator, mode);
              }}
	break;
	case 340:
#line 480 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
    }}
	break;
	case 341:
#line 487 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        // Accumulator is a negative offset.
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        if (accumulator >= groupIndex) {
            throw LocatedParseError("Invalid reference");
        }
        unsigned idx = groupIndex - accumulator;
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(idx));
    }}
	break;
	case 342:
#line 480 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
    }}
	break;
	case 343:
#line 487 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        // Accumulator is a negative offset.
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        if (accumulator >= groupIndex) {
            throw LocatedParseError("Invalid reference");
        }
        unsigned idx = groupIndex - accumulator;
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(idx));
    }}
	break;
	case 344:
#line 499 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(label));
    }}
	break;
	case 345:
#line 499 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(label));
    }}
	break;
	case 346:
#line 499 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(label));
    }}
	break;
	case 347:
#line 499 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(label));
    }}
	break;
	case 348:
#line 499 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(label));
    }}
	break;
	case 349:
#line 1550 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "Onigiruma subroutine call at index " << ts - ptr <<
                         " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 350:
#line 1561 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  string oct(ts + 3, te - ts - 4);
                  unsigned long val;
                  try {
                      val = stoul(oct, nullptr, 8);
                  } catch (const std::out_of_range &) {
                      val = MAX_UNICODE + 1;
                  }
                  if ((!mode.utf8 && val > 255) || val > MAX_UNICODE) {
                      throw LocatedParseError("Value in \\o{...} sequence is too large");
                  }
                  addEscapedOctal(currentSeq, (unichar)val, mode);
              }}
	break;
	case 351:
#line 1579 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addEscapedHex(currentSeq, accumulator, mode);
              }}
	break;
	case 352:
#line 1583 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  string hex(ts + 3, te - ts - 4);
                  unsigned long val;
                  try {
                      val = stoul(hex, nullptr, 16);
                  } catch (const std::out_of_range &) {
                      val = MAX_UNICODE + 1;
                  }
                  if (val > MAX_UNICODE) {
                      throw LocatedParseError("Value in \\x{...} sequence is too large");
                  }
                  addEscapedHex(currentSeq, (unichar)val, mode);
              }}
	break;
	case 353:
#line 1601 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (te - ts < 3) {
                      assert(te - ts == 2);
                      throw LocatedParseError(SLASH_C_ERROR);
                  } else {
                      assert(te - ts == 3);
                      addLiteral(currentSeq, decodeCtrl(ts[2]), mode);
                  }
              }}
	break;
	case 354:
#line 1611 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "'\\" << *(ts + 1) << "' at index " << ts - ptr
                      << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 355:
#line 1619 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_WORD, false, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 356:
#line 1624 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_WORD, true, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 357:
#line 1629 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_SPACE, false, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 358:
#line 1634 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_SPACE, true, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 359:
#line 1639 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_DIGIT, false, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 360:
#line 1644 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_DIGIT, true, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 361:
#line 1649 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_HORZ, false, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 362:
#line 1654 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_HORZ, true, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 363:
#line 1659 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_VERT, false, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 364:
#line 1664 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto cc = generateComponent(CLASS_VERT, true, mode);
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 365:
#line 1669 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(!currentCls && !inCharClass);
                  currentCls = getComponentClass(mode);
                  negated = false;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 559;goto _again;}}
              }}
	break;
	case 366:
#line 1677 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(!currentCls && !inCharClass);
                  currentCls = getComponentClass(mode);
                  negated = false;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 818;goto _again;}}
              }}
	break;
	case 367:
#line 1685 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(!currentCls && !inCharClass);
                  currentCls = getComponentClass(mode);
                  negated = true;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 559;goto _again;}}
              }}
	break;
	case 368:
#line 1693 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(!currentCls && !inCharClass);
                  currentCls = getComponentClass(mode);
                  negated = true;
                  p--;
                  {
        DEBUG_PRINTF("stack %zu top %d\n", stack.size(), top);
        if ((int)stack.size() == top) {
            stack.resize(2 * (top + 1));
        }
    {stack[top++] = cs; cs = 818;goto _again;}}
              }}
	break;
	case 369:
#line 1705 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "\\R at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 370:
#line 1712 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "\\K at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 371:
#line 1727 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "\\G at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 372:
#line 1733 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  currentSeq->addComponent(std::make_unique<ComponentEUS>(ts - ptr, mode));
              }}
	break;
	case 373:
#line 1738 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, *(ts + 1), mode);
              }}
	break;
	case 374:
#line 317 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{ 
        inComment = true;
        {cs = 848;goto _again;}
    }}
	break;
	case 375:
#line 434 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        mode = newMode;
        currentSeq->addComponent(std::make_unique<ComponentEmpty>());
    }}
	break;
	case 376:
#line 356 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        mode = newMode;
        currentSeq =
            enterSequence(currentSeq, std::make_unique<ComponentSequence>());
    }}
	break;
	case 377:
#line 370 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
            std::make_unique<ComponentAssertion>(ComponentAssertion::LOOKAHEAD,
                                                 ComponentAssertion::POS));
    }}
	break;
	case 378:
#line 376 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
            std::make_unique<ComponentAssertion>(ComponentAssertion::LOOKAHEAD,
                                                 ComponentAssertion::NEG));
    }}
	break;
	case 379:
#line 382 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
            std::make_unique<ComponentAssertion>(ComponentAssertion::LOOKBEHIND,
                                                 ComponentAssertion::POS));
    }}
	break;
	case 380:
#line 388 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
            std::make_unique<ComponentAssertion>(ComponentAssertion::LOOKBEHIND,
                                                 ComponentAssertion::NEG));
    }}
	break;
	case 381:
#line 394 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        throw LocatedParseError("Embedded code is not supported");
    }}
	break;
	case 382:
#line 394 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        throw LocatedParseError("Embedded code is not supported");
    }}
	break;
	case 383:
#line 417 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
                                   std::make_unique<ComponentAtomicGroup>());
    }}
	break;
	case 384:
#line 337 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        assert(!label.empty()); // should be guaranteed by machine
        char c = *label.begin();
        if (c >= '0' && c <= '9') {
            throw LocatedParseError("Group name cannot begin with a digit");
        }
        if (!groupNames.insert(label).second) {
            throw LocatedParseError("Two named subpatterns use the name '" + label + "'");
        }
        PUSH_SEQUENCE;
        auto seq = std::make_unique<ComponentSequence>();
        seq->setCaptureIndex(groupIndex++);
        seq->setCaptureName(label);
        currentSeq = enterSequence(currentSeq, std::move(seq));
    }}
	break;
	case 385:
#line 400 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        throw LocatedParseError("Subpattern reference unsupported");
    }}
	break;
	case 386:
#line 400 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        throw LocatedParseError("Subpattern reference unsupported");
    }}
	break;
	case 387:
#line 1784 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto a = std::make_unique<ComponentAssertion>(
                        ComponentAssertion::LOOKAHEAD, ComponentAssertion::POS);
                  ComponentAssertion *a_seq = a.get();
                  PUSH_SEQUENCE;
                  currentSeq = enterSequence(currentSeq,
                        std::make_unique<ComponentCondReference>(std::move(a)));
                  PUSH_SEQUENCE;
                  currentSeq = a_seq;
              }}
	break;
	case 388:
#line 1795 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto a = std::make_unique<ComponentAssertion>(
                        ComponentAssertion::LOOKAHEAD, ComponentAssertion::NEG);
                  ComponentAssertion *a_seq = a.get();
                  PUSH_SEQUENCE;
                  currentSeq = enterSequence(currentSeq,
                        std::make_unique<ComponentCondReference>(std::move(a)));
                  PUSH_SEQUENCE;
                  currentSeq = a_seq;
              }}
	break;
	case 389:
#line 1806 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto a = std::make_unique<ComponentAssertion>(
                      ComponentAssertion::LOOKBEHIND, ComponentAssertion::POS);
                  ComponentAssertion *a_seq = a.get();
                  PUSH_SEQUENCE;
                  currentSeq = enterSequence(currentSeq,
                        std::make_unique<ComponentCondReference>(std::move(a)));
                  PUSH_SEQUENCE;
                  currentSeq = a_seq;
              }}
	break;
	case 390:
#line 1817 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  auto a = std::make_unique<ComponentAssertion>(
                      ComponentAssertion::LOOKBEHIND, ComponentAssertion::NEG);
                  ComponentAssertion *a_seq = a.get();
                  PUSH_SEQUENCE;
                  currentSeq = enterSequence(currentSeq,
                        std::make_unique<ComponentCondReference>(std::move(a)));
                  PUSH_SEQUENCE;
                  currentSeq = a_seq;
              }}
	break;
	case 391:
#line 1829 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("Pattern recursion not supported");
              }}
	break;
	case 392:
#line 403 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        PUSH_SEQUENCE;
        currentSeq = enterSequence(currentSeq,
                std::make_unique<ComponentCondReference>(accumulator));
    }}
	break;
	case 393:
#line 411 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
        PUSH_SEQUENCE;
        assert(!label.empty());
        currentSeq = enterSequence(currentSeq,
                std::make_unique<ComponentCondReference>(label));
    }}
	break;
	case 394:
#line 1845 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  ostringstream str;
                  str << "Callout at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 395:
#line 1853 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  throw LocatedParseError("Unrecognised character after (?");
              }}
	break;
	case 396:
#line 1858 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint2c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 397:
#line 1867 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint3c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 398:
#line 1876 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  /* leverage ComponentClass to generate the vertices */
                  auto cc = getComponentClass(mode);
                  cc->add(readUtf8CodePoint4c(ts));
                  cc->finalize();
                  currentSeq->addComponent(std::move(cc));
              }}
	break;
	case 399:
#line 1885 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 400:
#line 1894 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  if (mode.ignore_space == false) {
                      addLiteral(currentSeq, *ts, mode);
                  }
              }}
	break;
	case 401:
#line 1899 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p+1;{
                  addLiteral(currentSeq, *ts, mode);
              }}
	break;
	case 402:
#line 329 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        PUSH_SEQUENCE;
        auto seq = std::make_unique<ComponentSequence>();
        seq->setCaptureIndex(groupIndex++);
        currentSeq = enterSequence(currentSeq, std::move(seq));
    }}
	break;
	case 403:
#line 422 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        assert(!currentCls);
        assert(!inCharClass); // not reentrant
        currentCls = getComponentClass(mode);
        inCharClass = true;
        inCharClassEarly = true;
        currentClsBegin = ts;
        {cs = 836;goto _again;}
    }}
	break;
	case 404:
#line 1311 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (!currentSeq->addRepeat(0, ComponentRepeat::NoLimit,
                                             ComponentRepeat::REPEAT_GREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 405:
#line 1332 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (!currentSeq->addRepeat(1, ComponentRepeat::NoLimit,
                                             ComponentRepeat::REPEAT_GREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 406:
#line 1353 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (!currentSeq->addRepeat(
                           0, 1, ComponentRepeat::REPEAT_GREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 407:
#line 1374 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (repeatN > repeatM || repeatM == 0) {
                      throwInvalidRepeat();
                  } else if (!currentSeq->addRepeat(
                                  repeatN, repeatM,
                                  ComponentRepeat::REPEAT_GREEDY)) {
                      throwInvalidRepeat();
                  }
              }}
	break;
	case 408:
#line 1489 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  addLiteral(currentSeq, octAccumulator, mode);
              }}
	break;
	case 409:
#line 1492 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  // If there are enough capturing sub expressions, this may be
                  // a back reference
                  accumulator = parseAsDecimal(octAccumulator);
                  if (accumulator < groupIndex) {
                      currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
                  } else {
                      addEscapedOctal(currentSeq, octAccumulator, mode);
                  }
              }}
	break;
	case 410:
#line 480 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
    }}
	break;
	case 411:
#line 480 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
    }}
	break;
	case 412:
#line 487 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        // Accumulator is a negative offset.
        if (accumulator == 0) {
            throw LocatedParseError("Numbered reference cannot be zero");
        }
        if (accumulator >= groupIndex) {
            throw LocatedParseError("Invalid reference");
        }
        unsigned idx = groupIndex - accumulator;
        currentSeq->addComponent(std::make_unique<ComponentBackReference>(idx));
    }}
	break;
	case 413:
#line 1558 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Invalid reference after \\g");
              }}
	break;
	case 414:
#line 1575 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Value in \\o{...} sequence is non-octal or missing braces");
              }}
	break;
	case 415:
#line 1579 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  addEscapedHex(currentSeq, accumulator, mode);
              }}
	break;
	case 416:
#line 1597 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Value in \\x{...} sequence is non-hex or missing }");
              }}
	break;
	case 417:
#line 1601 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  if (te - ts < 3) {
                      assert(te - ts == 2);
                      throw LocatedParseError(SLASH_C_ERROR);
                  } else {
                      assert(te - ts == 3);
                      addLiteral(currentSeq, decodeCtrl(ts[2]), mode);
                  }
              }}
	break;
	case 418:
#line 1701 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ throw LocatedParseError("Malformed property"); }}
	break;
	case 419:
#line 1702 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{ throw LocatedParseError("Malformed property"); }}
	break;
	case 420:
#line 1720 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  ostringstream str;
                  str << "\\k at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 421:
#line 1743 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  assert(ts + 1 == pe);
                  ostringstream str;
                  str << "Unescaped \\ at end of input, index " << ts - ptr << ".";
                  throw ParseError(str.str());
              }}
	break;
	case 422:
#line 397 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
        throw LocatedParseError("Conditional subpattern unsupported");
    }}
	break;
	case 423:
#line 1853 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  throw LocatedParseError("Unrecognised character after (?");
              }}
	break;
	case 424:
#line 1885 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 425:
#line 1899 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{te = p;p--;{
                  addLiteral(currentSeq, *ts, mode);
              }}
	break;
	case 426:
#line 329 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
        PUSH_SEQUENCE;
        auto seq = std::make_unique<ComponentSequence>();
        seq->setCaptureIndex(groupIndex++);
        currentSeq = enterSequence(currentSeq, std::move(seq));
    }}
	break;
	case 427:
#line 422 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
        assert(!currentCls);
        assert(!inCharClass); // not reentrant
        currentCls = getComponentClass(mode);
        inCharClass = true;
        inCharClassEarly = true;
        currentClsBegin = ts;
        {cs = 836;goto _again;}
    }}
	break;
	case 428:
#line 1558 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Invalid reference after \\g");
              }}
	break;
	case 429:
#line 1575 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Value in \\o{...} sequence is non-octal or missing braces");
              }}
	break;
	case 430:
#line 1597 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Value in \\x{...} sequence is non-hex or missing }");
              }}
	break;
	case 431:
#line 1720 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  ostringstream str;
                  str << "\\k at index " << ts - ptr << " not supported.";
                  throw ParseError(str.str());
              }}
	break;
	case 432:
#line 397 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
        throw LocatedParseError("Conditional subpattern unsupported");
    }}
	break;
	case 433:
#line 1853 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  throw LocatedParseError("Unrecognised character after (?");
              }}
	break;
	case 434:
#line 1885 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  assert(mode.utf8);
                  throwInvalidUtf8();
              }}
	break;
	case 435:
#line 1899 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{{p = ((te))-1;}{
                  addLiteral(currentSeq, *ts, mode);
              }}
	break;
	case 436:
#line 1 "NONE"
	{	switch( act ) {
	case 288:
	{{p = ((te))-1;}
                  // If there are enough capturing sub expressions, this may be
                  // a back reference
                  accumulator = parseAsDecimal(octAccumulator);
                  if (accumulator < groupIndex) {
                      currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
                  } else {
                      addEscapedOctal(currentSeq, octAccumulator, mode);
                  }
              }
	break;
	case 290:
	{{p = ((te))-1;}
                  // if there are enough left parens to this point, back ref
                  if (accumulator < groupIndex) {
                      currentSeq->addComponent(std::make_unique<ComponentBackReference>(accumulator));
                  } else {
                      // Otherwise, we interpret the first three digits as an
                      // octal escape, and the remaining characters stand for
                      // themselves as literals.
                      const char *s = ts;
                      unsigned int accum = 0;
                      unsigned int oct_digits = 0;
                      assert(*s == '\\'); // token starts at backslash
                      for (++s; s < te && oct_digits < 3; ++oct_digits, ++s) {
                          u8 digit = *s - '0';
                          if (digit < 8) {
                              accum = digit + accum * 8;
                          } else {
                              break;
                          }
                      }

                      if (oct_digits > 0) {
                          addEscapedOctal(currentSeq, accum, mode);
                      }

                      // And then the rest of the digits, if any, are literal.
                      for (; s < te; ++s) {
                          addLiteral(currentSeq, *s, mode);
                      }
                  }
              }
	break;
	case 330:
	{{p = ((te))-1;}
                  addLiteral(currentSeq, *(ts + 1), mode);
              }
	break;
	}
	}
	break;
#line 10186 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
		}
	}
	goto _again;

_again:
	_acts = _regex_actions + _regex_to_state_actions[cs];
	_nacts = (unsigned int) *_acts++;
	while ( _nacts-- > 0 ) {
		switch ( *_acts++ ) {
	case 23:
#line 1 "NONE"
	{ts = 0;}
	break;
#line 10198 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
		}
	}

	if ( cs == 0 )
		goto _out;
	if ( ++p != pe )
		goto _resume;
	_test_eof: {}
	if ( p == eof )
	{
	switch ( cs ) {
	case 747: goto tr894;
	case 1: goto tr0;
	case 2: goto tr0;
	case 748: goto tr897;
	case 3: goto tr21;
	case 749: goto tr897;
	case 4: goto tr21;
	case 750: goto tr898;
	case 5: goto tr26;
	case 6: goto tr26;
	case 7: goto tr26;
	case 8: goto tr26;
	case 9: goto tr26;
	case 10: goto tr26;
	case 11: goto tr26;
	case 12: goto tr26;
	case 13: goto tr26;
	case 14: goto tr26;
	case 15: goto tr26;
	case 16: goto tr26;
	case 17: goto tr26;
	case 18: goto tr26;
	case 751: goto tr897;
	case 19: goto tr21;
	case 752: goto tr897;
	case 20: goto tr21;
	case 753: goto tr897;
	case 754: goto tr897;
	case 21: goto tr21;
	case 755: goto tr897;
	case 756: goto tr897;
	case 22: goto tr21;
	case 757: goto tr897;
	case 23: goto tr21;
	case 24: goto tr21;
	case 25: goto tr21;
	case 26: goto tr21;
	case 758: goto tr897;
	case 27: goto tr21;
	case 28: goto tr21;
	case 759: goto tr911;
	case 760: goto tr914;
	case 761: goto tr917;
	case 762: goto tr920;
	case 29: goto tr55;
	case 30: goto tr55;
	case 31: goto tr55;
	case 32: goto tr55;
	case 33: goto tr55;
	case 34: goto tr55;
	case 35: goto tr55;
	case 36: goto tr55;
	case 37: goto tr55;
	case 763: goto tr921;
	case 764: goto tr962;
	case 765: goto tr962;
	case 766: goto tr965;
	case 767: goto tr968;
	case 768: goto tr970;
	case 769: goto tr971;
	case 770: goto tr974;
	case 771: goto tr976;
	case 38: goto tr67;
	case 39: goto tr67;
	case 772: goto tr980;
	case 773: goto tr980;
	case 774: goto tr983;
	case 775: goto tr983;
	case 40: goto tr67;
	case 41: goto tr67;
	case 42: goto tr67;
	case 43: goto tr67;
	case 44: goto tr67;
	case 45: goto tr67;
	case 46: goto tr67;
	case 47: goto tr67;
	case 48: goto tr67;
	case 49: goto tr67;
	case 776: goto tr986;
	case 50: goto tr83;
	case 51: goto tr83;
	case 52: goto tr83;
	case 53: goto tr83;
	case 54: goto tr83;
	case 55: goto tr83;
	case 777: goto tr990;
	case 56: goto tr90;
	case 57: goto tr90;
	case 778: goto tr992;
	case 779: goto tr995;
	case 780: goto tr995;
	case 781: goto tr1003;
	case 58: goto tr93;
	case 782: goto tr1004;
	case 59: goto tr96;
	case 60: goto tr96;
	case 61: goto tr96;
	case 783: goto tr1005;
	case 784: goto tr1008;
	case 785: goto tr1008;
	case 62: goto tr103;
	case 786: goto tr1008;
	case 63: goto tr103;
	case 64: goto tr103;
	case 788: goto tr1015;
	case 65: goto tr107;
	case 789: goto tr1015;
	case 66: goto tr107;
	case 67: goto tr107;
	case 68: goto tr107;
	case 69: goto tr107;
	case 70: goto tr107;
	case 791: goto tr1042;
	case 71: goto tr116;
	case 72: goto tr116;
	case 73: goto tr116;
	case 74: goto tr116;
	case 75: goto tr116;
	case 76: goto tr116;
	case 77: goto tr116;
	case 78: goto tr116;
	case 79: goto tr116;
	case 80: goto tr116;
	case 81: goto tr116;
	case 82: goto tr116;
	case 83: goto tr116;
	case 84: goto tr116;
	case 85: goto tr116;
	case 792: goto tr1042;
	case 86: goto tr116;
	case 87: goto tr116;
	case 88: goto tr116;
	case 89: goto tr116;
	case 90: goto tr116;
	case 91: goto tr116;
	case 92: goto tr116;
	case 93: goto tr116;
	case 94: goto tr116;
	case 95: goto tr116;
	case 96: goto tr116;
	case 97: goto tr116;
	case 98: goto tr116;
	case 99: goto tr116;
	case 100: goto tr116;
	case 101: goto tr116;
	case 102: goto tr116;
	case 103: goto tr116;
	case 104: goto tr116;
	case 105: goto tr116;
	case 106: goto tr116;
	case 107: goto tr116;
	case 108: goto tr116;
	case 109: goto tr116;
	case 110: goto tr116;
	case 111: goto tr116;
	case 112: goto tr116;
	case 113: goto tr116;
	case 114: goto tr116;
	case 115: goto tr116;
	case 116: goto tr116;
	case 117: goto tr116;
	case 118: goto tr116;
	case 119: goto tr116;
	case 120: goto tr116;
	case 121: goto tr116;
	case 793: goto tr1051;
	case 122: goto tr173;
	case 123: goto tr173;
	case 124: goto tr173;
	case 125: goto tr173;
	case 126: goto tr173;
	case 127: goto tr173;
	case 128: goto tr173;
	case 129: goto tr173;
	case 130: goto tr173;
	case 131: goto tr173;
	case 132: goto tr173;
	case 133: goto tr173;
	case 134: goto tr173;
	case 135: goto tr173;
	case 136: goto tr173;
	case 137: goto tr173;
	case 138: goto tr173;
	case 139: goto tr173;
	case 140: goto tr173;
	case 141: goto tr173;
	case 142: goto tr173;
	case 143: goto tr173;
	case 144: goto tr173;
	case 145: goto tr173;
	case 146: goto tr173;
	case 147: goto tr173;
	case 148: goto tr173;
	case 794: goto tr1061;
	case 149: goto tr203;
	case 150: goto tr203;
	case 151: goto tr203;
	case 152: goto tr203;
	case 153: goto tr203;
	case 154: goto tr203;
	case 155: goto tr173;
	case 156: goto tr173;
	case 157: goto tr173;
	case 158: goto tr173;
	case 159: goto tr173;
	case 160: goto tr173;
	case 161: goto tr173;
	case 162: goto tr173;
	case 163: goto tr173;
	case 164: goto tr173;
	case 165: goto tr173;
	case 166: goto tr173;
	case 167: goto tr173;
	case 168: goto tr173;
	case 169: goto tr173;
	case 170: goto tr173;
	case 171: goto tr173;
	case 795: goto tr1042;
	case 172: goto tr116;
	case 173: goto tr116;
	case 174: goto tr116;
	case 175: goto tr116;
	case 176: goto tr116;
	case 177: goto tr116;
	case 178: goto tr116;
	case 179: goto tr116;
	case 180: goto tr116;
	case 181: goto tr116;
	case 182: goto tr116;
	case 183: goto tr116;
	case 796: goto tr1042;
	case 184: goto tr116;
	case 185: goto tr116;
	case 186: goto tr116;
	case 187: goto tr116;
	case 188: goto tr116;
	case 189: goto tr116;
	case 190: goto tr116;
	case 191: goto tr116;
	case 192: goto tr116;
	case 193: goto tr116;
	case 194: goto tr116;
	case 195: goto tr116;
	case 196: goto tr116;
	case 197: goto tr116;
	case 198: goto tr116;
	case 199: goto tr116;
	case 200: goto tr116;
	case 201: goto tr116;
	case 202: goto tr116;
	case 203: goto tr116;
	case 204: goto tr116;
	case 205: goto tr116;
	case 206: goto tr116;
	case 207: goto tr116;
	case 797: goto tr1042;
	case 208: goto tr116;
	case 209: goto tr116;
	case 210: goto tr116;
	case 211: goto tr116;
	case 212: goto tr116;
	case 213: goto tr116;
	case 214: goto tr116;
	case 215: goto tr116;
	case 216: goto tr116;
	case 217: goto tr116;
	case 218: goto tr116;
	case 219: goto tr116;
	case 220: goto tr116;
	case 221: goto tr116;
	case 222: goto tr116;
	case 223: goto tr116;
	case 224: goto tr116;
	case 225: goto tr116;
	case 226: goto tr116;
	case 227: goto tr116;
	case 228: goto tr116;
	case 229: goto tr116;
	case 230: goto tr116;
	case 231: goto tr116;
	case 232: goto tr116;
	case 233: goto tr116;
	case 234: goto tr116;
	case 235: goto tr116;
	case 236: goto tr116;
	case 237: goto tr116;
	case 238: goto tr116;
	case 239: goto tr116;
	case 798: goto tr1042;
	case 240: goto tr116;
	case 799: goto tr1075;
	case 241: goto tr299;
	case 242: goto tr299;
	case 243: goto tr299;
	case 244: goto tr299;
	case 245: goto tr299;
	case 246: goto tr116;
	case 247: goto tr116;
	case 248: goto tr116;
	case 249: goto tr116;
	case 250: goto tr116;
	case 251: goto tr116;
	case 252: goto tr116;
	case 253: goto tr116;
	case 254: goto tr116;
	case 255: goto tr116;
	case 800: goto tr1042;
	case 256: goto tr116;
	case 257: goto tr116;
	case 258: goto tr116;
	case 259: goto tr116;
	case 260: goto tr116;
	case 261: goto tr116;
	case 262: goto tr116;
	case 263: goto tr116;
	case 264: goto tr116;
	case 265: goto tr116;
	case 266: goto tr116;
	case 267: goto tr116;
	case 268: goto tr116;
	case 269: goto tr116;
	case 270: goto tr116;
	case 271: goto tr116;
	case 272: goto tr116;
	case 273: goto tr116;
	case 274: goto tr116;
	case 275: goto tr116;
	case 276: goto tr116;
	case 277: goto tr116;
	case 278: goto tr116;
	case 279: goto tr116;
	case 280: goto tr116;
	case 281: goto tr116;
	case 282: goto tr116;
	case 283: goto tr116;
	case 284: goto tr116;
	case 285: goto tr116;
	case 286: goto tr116;
	case 287: goto tr116;
	case 288: goto tr116;
	case 289: goto tr116;
	case 290: goto tr116;
	case 291: goto tr116;
	case 292: goto tr116;
	case 293: goto tr116;
	case 294: goto tr116;
	case 295: goto tr116;
	case 296: goto tr116;
	case 297: goto tr116;
	case 298: goto tr116;
	case 299: goto tr116;
	case 801: goto tr1042;
	case 300: goto tr116;
	case 301: goto tr116;
	case 302: goto tr116;
	case 303: goto tr116;
	case 304: goto tr116;
	case 305: goto tr116;
	case 802: goto tr1042;
	case 306: goto tr116;
	case 307: goto tr116;
	case 308: goto tr116;
	case 309: goto tr116;
	case 310: goto tr116;
	case 311: goto tr116;
	case 312: goto tr116;
	case 313: goto tr116;
	case 314: goto tr116;
	case 315: goto tr116;
	case 316: goto tr116;
	case 317: goto tr116;
	case 318: goto tr116;
	case 319: goto tr116;
	case 320: goto tr116;
	case 321: goto tr116;
	case 322: goto tr116;
	case 323: goto tr116;
	case 324: goto tr116;
	case 325: goto tr116;
	case 326: goto tr116;
	case 327: goto tr116;
	case 328: goto tr116;
	case 329: goto tr116;
	case 330: goto tr116;
	case 331: goto tr116;
	case 332: goto tr116;
	case 333: goto tr116;
	case 803: goto tr1083;
	case 334: goto tr399;
	case 335: goto tr399;
	case 336: goto tr399;
	case 337: goto tr399;
	case 338: goto tr399;
	case 339: goto tr399;
	case 340: goto tr399;
	case 341: goto tr399;
	case 342: goto tr399;
	case 343: goto tr399;
	case 344: goto tr399;
	case 345: goto tr399;
	case 346: goto tr399;
	case 347: goto tr399;
	case 348: goto tr399;
	case 349: goto tr399;
	case 350: goto tr399;
	case 351: goto tr399;
	case 352: goto tr399;
	case 353: goto tr399;
	case 354: goto tr399;
	case 355: goto tr399;
	case 356: goto tr399;
	case 804: goto tr1094;
	case 357: goto tr427;
	case 358: goto tr427;
	case 359: goto tr427;
	case 360: goto tr427;
	case 361: goto tr427;
	case 362: goto tr427;
	case 363: goto tr427;
	case 364: goto tr427;
	case 365: goto tr427;
	case 366: goto tr427;
	case 367: goto tr427;
	case 805: goto tr1101;
	case 368: goto tr440;
	case 369: goto tr440;
	case 370: goto tr440;
	case 371: goto tr440;
	case 372: goto tr440;
	case 373: goto tr440;
	case 374: goto tr440;
	case 375: goto tr440;
	case 376: goto tr440;
	case 377: goto tr427;
	case 378: goto tr427;
	case 379: goto tr427;
	case 380: goto tr427;
	case 381: goto tr427;
	case 382: goto tr427;
	case 383: goto tr427;
	case 384: goto tr427;
	case 385: goto tr427;
	case 386: goto tr427;
	case 387: goto tr427;
	case 388: goto tr427;
	case 806: goto tr1103;
	case 389: goto tr462;
	case 390: goto tr462;
	case 391: goto tr462;
	case 392: goto tr462;
	case 393: goto tr462;
	case 394: goto tr462;
	case 395: goto tr462;
	case 396: goto tr462;
	case 397: goto tr462;
	case 398: goto tr462;
	case 807: goto tr1042;
	case 399: goto tr116;
	case 400: goto tr116;
	case 401: goto tr116;
	case 402: goto tr116;
	case 403: goto tr116;
	case 404: goto tr116;
	case 405: goto tr116;
	case 406: goto tr116;
	case 407: goto tr116;
	case 408: goto tr116;
	case 409: goto tr116;
	case 410: goto tr116;
	case 411: goto tr116;
	case 412: goto tr116;
	case 413: goto tr116;
	case 414: goto tr116;
	case 415: goto tr116;
	case 416: goto tr116;
	case 417: goto tr116;
	case 418: goto tr116;
	case 419: goto tr116;
	case 420: goto tr116;
	case 421: goto tr116;
	case 422: goto tr116;
	case 423: goto tr116;
	case 424: goto tr116;
	case 425: goto tr116;
	case 426: goto tr116;
	case 427: goto tr116;
	case 428: goto tr116;
	case 429: goto tr116;
	case 430: goto tr116;
	case 431: goto tr116;
	case 432: goto tr116;
	case 433: goto tr116;
	case 434: goto tr116;
	case 435: goto tr116;
	case 436: goto tr116;
	case 437: goto tr116;
	case 438: goto tr116;
	case 439: goto tr116;
	case 440: goto tr116;
	case 441: goto tr116;
	case 442: goto tr116;
	case 443: goto tr116;
	case 444: goto tr116;
	case 445: goto tr116;
	case 808: goto tr1113;
	case 446: goto tr524;
	case 447: goto tr524;
	case 448: goto tr524;
	case 449: goto tr524;
	case 450: goto tr524;
	case 451: goto tr524;
	case 452: goto tr524;
	case 453: goto tr524;
	case 454: goto tr524;
	case 455: goto tr524;
	case 456: goto tr524;
	case 457: goto tr524;
	case 458: goto tr524;
	case 809: goto tr1042;
	case 459: goto tr116;
	case 460: goto tr116;
	case 461: goto tr116;
	case 462: goto tr116;
	case 463: goto tr116;
	case 464: goto tr116;
	case 465: goto tr116;
	case 810: goto tr1124;
	case 466: goto tr546;
	case 467: goto tr546;
	case 468: goto tr546;
	case 469: goto tr546;
	case 470: goto tr546;
	case 471: goto tr546;
	case 472: goto tr546;
	case 473: goto tr546;
	case 474: goto tr546;
	case 475: goto tr546;
	case 476: goto tr546;
	case 477: goto tr546;
	case 478: goto tr546;
	case 479: goto tr546;
	case 480: goto tr546;
	case 481: goto tr546;
	case 482: goto tr546;
	case 483: goto tr546;
	case 484: goto tr546;
	case 485: goto tr546;
	case 486: goto tr546;
	case 487: goto tr546;
	case 488: goto tr546;
	case 489: goto tr546;
	case 490: goto tr546;
	case 491: goto tr546;
	case 492: goto tr546;
	case 493: goto tr546;
	case 494: goto tr546;
	case 495: goto tr546;
	case 496: goto tr546;
	case 497: goto tr546;
	case 498: goto tr546;
	case 499: goto tr546;
	case 500: goto tr546;
	case 501: goto tr546;
	case 502: goto tr546;
	case 503: goto tr546;
	case 504: goto tr546;
	case 505: goto tr546;
	case 506: goto tr546;
	case 507: goto tr546;
	case 508: goto tr546;
	case 509: goto tr546;
	case 811: goto tr1042;
	case 510: goto tr116;
	case 511: goto tr116;
	case 512: goto tr116;
	case 513: goto tr116;
	case 514: goto tr116;
	case 515: goto tr116;
	case 516: goto tr116;
	case 517: goto tr116;
	case 518: goto tr116;
	case 519: goto tr116;
	case 520: goto tr116;
	case 521: goto tr116;
	case 522: goto tr116;
	case 523: goto tr116;
	case 524: goto tr116;
	case 525: goto tr116;
	case 526: goto tr116;
	case 527: goto tr116;
	case 528: goto tr116;
	case 529: goto tr116;
	case 530: goto tr116;
	case 531: goto tr116;
	case 532: goto tr116;
	case 533: goto tr116;
	case 534: goto tr116;
	case 535: goto tr116;
	case 536: goto tr116;
	case 537: goto tr116;
	case 538: goto tr116;
	case 539: goto tr116;
	case 540: goto tr116;
	case 541: goto tr116;
	case 542: goto tr116;
	case 543: goto tr116;
	case 544: goto tr116;
	case 545: goto tr116;
	case 546: goto tr116;
	case 547: goto tr116;
	case 812: goto tr1042;
	case 548: goto tr116;
	case 549: goto tr116;
	case 550: goto tr116;
	case 551: goto tr116;
	case 552: goto tr116;
	case 553: goto tr116;
	case 813: goto tr1042;
	case 554: goto tr116;
	case 814: goto tr1042;
	case 555: goto tr116;
	case 556: goto tr116;
	case 557: goto tr116;
	case 558: goto tr116;
	case 815: goto tr1042;
	case 816: goto tr1145;
	case 820: goto tr1166;
	case 563: goto tr654;
	case 564: goto tr654;
	case 565: goto tr654;
	case 566: goto tr654;
	case 567: goto tr654;
	case 568: goto tr654;
	case 569: goto tr654;
	case 570: goto tr654;
	case 571: goto tr654;
	case 572: goto tr654;
	case 573: goto tr654;
	case 574: goto tr654;
	case 575: goto tr654;
	case 576: goto tr654;
	case 577: goto tr654;
	case 578: goto tr654;
	case 579: goto tr654;
	case 580: goto tr654;
	case 581: goto tr654;
	case 582: goto tr654;
	case 583: goto tr654;
	case 584: goto tr654;
	case 585: goto tr654;
	case 586: goto tr654;
	case 587: goto tr654;
	case 588: goto tr654;
	case 589: goto tr654;
	case 590: goto tr654;
	case 591: goto tr654;
	case 592: goto tr654;
	case 593: goto tr654;
	case 594: goto tr654;
	case 595: goto tr654;
	case 596: goto tr654;
	case 597: goto tr654;
	case 598: goto tr654;
	case 599: goto tr654;
	case 600: goto tr654;
	case 601: goto tr654;
	case 602: goto tr654;
	case 603: goto tr654;
	case 604: goto tr654;
	case 605: goto tr654;
	case 606: goto tr654;
	case 607: goto tr654;
	case 608: goto tr654;
	case 609: goto tr654;
	case 610: goto tr654;
	case 611: goto tr654;
	case 612: goto tr654;
	case 613: goto tr654;
	case 614: goto tr654;
	case 615: goto tr654;
	case 616: goto tr654;
	case 617: goto tr654;
	case 618: goto tr654;
	case 619: goto tr654;
	case 620: goto tr654;
	case 621: goto tr654;
	case 622: goto tr654;
	case 623: goto tr654;
	case 624: goto tr654;
	case 625: goto tr654;
	case 626: goto tr654;
	case 627: goto tr654;
	case 628: goto tr654;
	case 629: goto tr654;
	case 630: goto tr654;
	case 631: goto tr654;
	case 632: goto tr654;
	case 633: goto tr654;
	case 634: goto tr654;
	case 635: goto tr654;
	case 636: goto tr654;
	case 637: goto tr654;
	case 638: goto tr654;
	case 639: goto tr654;
	case 640: goto tr654;
	case 641: goto tr654;
	case 642: goto tr654;
	case 643: goto tr654;
	case 644: goto tr654;
	case 645: goto tr654;
	case 646: goto tr654;
	case 647: goto tr654;
	case 648: goto tr654;
	case 649: goto tr654;
	case 650: goto tr654;
	case 651: goto tr654;
	case 652: goto tr654;
	case 653: goto tr654;
	case 654: goto tr654;
	case 655: goto tr654;
	case 656: goto tr654;
	case 657: goto tr654;
	case 658: goto tr654;
	case 659: goto tr654;
	case 660: goto tr654;
	case 661: goto tr654;
	case 662: goto tr654;
	case 663: goto tr654;
	case 664: goto tr654;
	case 665: goto tr654;
	case 666: goto tr654;
	case 667: goto tr654;
	case 668: goto tr654;
	case 669: goto tr654;
	case 670: goto tr654;
	case 671: goto tr654;
	case 672: goto tr654;
	case 673: goto tr654;
	case 674: goto tr654;
	case 675: goto tr654;
	case 676: goto tr654;
	case 677: goto tr654;
	case 678: goto tr654;
	case 679: goto tr654;
	case 680: goto tr654;
	case 681: goto tr654;
	case 682: goto tr654;
	case 683: goto tr654;
	case 684: goto tr654;
	case 685: goto tr654;
	case 686: goto tr654;
	case 687: goto tr654;
	case 688: goto tr654;
	case 689: goto tr654;
	case 690: goto tr654;
	case 691: goto tr654;
	case 692: goto tr654;
	case 693: goto tr654;
	case 694: goto tr654;
	case 695: goto tr654;
	case 696: goto tr654;
	case 697: goto tr654;
	case 698: goto tr654;
	case 699: goto tr654;
	case 700: goto tr654;
	case 701: goto tr654;
	case 702: goto tr654;
	case 703: goto tr654;
	case 704: goto tr654;
	case 705: goto tr654;
	case 706: goto tr654;
	case 707: goto tr654;
	case 708: goto tr654;
	case 709: goto tr654;
	case 710: goto tr654;
	case 711: goto tr654;
	case 712: goto tr654;
	case 713: goto tr654;
	case 714: goto tr654;
	case 715: goto tr654;
	case 716: goto tr654;
	case 717: goto tr654;
	case 718: goto tr654;
	case 719: goto tr654;
	case 720: goto tr654;
	case 721: goto tr654;
	case 722: goto tr654;
	case 723: goto tr654;
	case 724: goto tr654;
	case 725: goto tr654;
	case 726: goto tr654;
	case 727: goto tr654;
	case 728: goto tr654;
	case 729: goto tr654;
	case 730: goto tr654;
	case 731: goto tr654;
	case 732: goto tr654;
	case 733: goto tr654;
	case 821: goto tr1166;
	case 822: goto tr1199;
	case 823: goto tr1199;
	case 824: goto tr1202;
	case 825: goto tr1202;
	case 826: goto tr1205;
	case 827: goto tr1208;
	case 828: goto tr1210;
	case 734: goto tr855;
	case 735: goto tr855;
	case 829: goto tr1212;
	case 830: goto tr1215;
	case 831: goto tr1215;
	case 832: goto tr1223;
	case 736: goto tr858;
	case 833: goto tr1224;
	case 834: goto tr1224;
	case 737: goto tr861;
	case 835: goto tr1224;
	case 738: goto tr861;
	case 739: goto tr861;
	case 837: goto tr1232;
	case 839: goto tr1241;
	case 840: goto tr1243;
	case 841: goto tr1243;
	case 740: goto tr865;
	case 842: goto tr1243;
	case 741: goto tr865;
	case 742: goto tr865;
	case 844: goto tr1253;
	case 845: goto tr1255;
	case 846: goto tr1255;
	case 743: goto tr869;
	case 847: goto tr1255;
	case 744: goto tr869;
	case 745: goto tr869;
	}
	const short *__acts = _regex_actions + _regex_eof_actions[cs];
	unsigned int __nacts = (unsigned int) *__acts++;
	while ( __nacts-- > 0 ) {
		switch ( *__acts++ ) {
	case 22:
#line 731 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"
	{ throw LocatedParseError("Malformed property"); }
	break;
#line 11051 "/home/infominer/codebase/vectorscan/build/src/parser/Parser.cpp"
		}
	}
	}

	_out: {}
	}

#line 1985 "/home/infominer/codebase/vectorscan/src/parser/Parser.rl"

        if (p != pe && *p != '\0') {
            // didn't make it to the end of our input, but we didn't throw a ParseError?
            assert(0);
            ostringstream str;
            str << "Parse error at index " << (p - ptr) << ".";
            throw ParseError(str.str());
        }

        if (currentCls) {
            assert(inCharClass);
            assert(currentClsBegin);
            ostringstream oss;
            oss << "Unterminated character class starting at index "
                << currentClsBegin - ptr << ".";
            throw ParseError(oss.str());
        }

        if (inComment) {
            throw ParseError("Unterminated comment.");
        }

        if (!sequences.empty()) {
            ostringstream str;
            str << "Missing close parenthesis for group started at index "
                << sequences.back().seqOffset << ".";
            throw ParseError(str.str());
        }

        // Unlikely, but possible
        if (groupIndex > 65535) {
            throw ParseError("The maximum number of capturing subexpressions is 65535.");
        }

        // Finalize the top-level sequence, which will take care of any
        // top-level alternation.
        currentSeq->finalize();
        assert(currentSeq == rootSeq.get());

        // Ensure that all references are valid.
        checkReferences(*rootSeq, groupIndex, groupNames);

        return rootSeq;
    } catch (LocatedParseError &error) {
        if (ts >= ptr && ts <= pe) {
            error.locate(ts - ptr);
        } else {
            error.locate(0);
        }
        throw;
    }
}

} // namespace ue2

#pragma clang diagnostic pop
