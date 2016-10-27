/*
 * Copyright (c) 2016, Intel Corporation
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

#include "config.h"

#include "shengdump.h"

#include "accel_dump.h"
#include "nfa_dump_internal.h"
#include "nfa_internal.h"
#include "sheng_internal.h"
#include "rdfa.h"
#include "ue2common.h"
#include "util/charreach.h"
#include "util/dump_charclass.h"
#include "util/simd_utils.h"


#ifndef DUMP_SUPPORT
#error No dump support!
#endif

using namespace std;

namespace ue2 {

static
const sstate_aux *get_aux(const NFA *n, dstate_id_t i) {
    assert(n && isShengType(n->type));

    const sheng *s = (const sheng *)getImplNfa(n);
    const sstate_aux *aux_base =
        (const sstate_aux *)((const char *)n + s->aux_offset);

    const sstate_aux *aux = aux_base + i;

    assert((const char *)aux < (const char *)s + s->length);

    return aux;
}

static
void dumpHeader(FILE *f, const sheng *s) {
    fprintf(f, "number of states: %u, DFA engine size: %u\n", s->n_states,
            s->length);
    fprintf(f, "aux base offset: %u, reports base offset: %u, "
               "accel offset: %u\n",
            s->aux_offset, s->report_offset, s->accel_offset);
    fprintf(f, "anchored start state: %u, floating start state: %u\n",
            s->anchored & SHENG_STATE_MASK, s->floating & SHENG_STATE_MASK);
    fprintf(f, "has accel: %u can die: %u single report: %u\n",
            !!(s->flags & SHENG_FLAG_HAS_ACCEL),
            !!(s->flags & SHENG_FLAG_CAN_DIE),
            !!(s->flags & SHENG_FLAG_SINGLE_REPORT));
}

static
void dumpAux(FILE *f, u32 state, const sstate_aux *aux) {
    fprintf(f, "state id: %u, reports offset: %u, EOD reports offset: %u, "
               "accel offset: %u, top: %u\n",
            state, aux->accept, aux->accept_eod, aux->accel,
            aux->top & SHENG_STATE_MASK);
}

static
void dumpReports(FILE *f, const report_list *rl) {
    fprintf(f, "reports count: %u\n", rl->count);
    for (u32 i = 0; i < rl->count; i++) {
        fprintf(f, "  report: %u, report ID: %u\n", i, rl->report[i]);
    }
}

static
void dumpMasks(FILE *f, const sheng *s) {
    for (u32 chr = 0; chr < 256; chr++) {
        u8 buf[16];
        m128 shuffle_mask = s->shuffle_masks[chr];
        store128(buf, shuffle_mask);

        fprintf(f, "%3u: ", chr);
        for (u32 pos = 0; pos < 16; pos++) {
            u8 c = buf[pos];
            if (c & SHENG_STATE_FLAG_MASK) {
                fprintf(f, "%2u* ", c & SHENG_STATE_MASK);
            } else {
                fprintf(f, "%2u  ", c & SHENG_STATE_MASK);
            }
        }
        fprintf(f, "\n");
    }
}

void nfaExecSheng_dumpText(const NFA *nfa, FILE *f) {
    assert(nfa->type == SHENG_NFA);
    const sheng *s = (const sheng *)getImplNfa(nfa);

    fprintf(f, "sheng DFA\n");
    dumpHeader(f, s);

    for (u32 state = 0; state < s->n_states; state++) {
        const sstate_aux *aux = get_aux(nfa, state);
        dumpAux(f, state, aux);
        if (aux->accept) {
            fprintf(f, "report list:\n");
            const report_list *rl =
                (const report_list *)((const char *)nfa + aux->accept);
            dumpReports(f, rl);
        }
        if (aux->accept_eod) {
            fprintf(f, "EOD report list:\n");
            const report_list *rl =
                (const report_list *)((const char *)nfa + aux->accept_eod);
            dumpReports(f, rl);
        }
        if (aux->accel) {
            fprintf(f, "accel:\n");
            const AccelAux *accel =
                (const AccelAux *)((const char *)nfa + aux->accel);
            dumpAccelInfo(f, *accel);
        }
    }

    fprintf(f, "\n");

    dumpMasks(f, s);

    fprintf(f, "\n");
}

static
void dumpDotPreambleDfa(FILE *f) {
    dumpDotPreamble(f);

    // DFA specific additions.
    fprintf(f, "STARTF [style=invis];\n");
    fprintf(f, "STARTA [style=invis];\n");
    fprintf(f, "0 [style=invis];\n");
}

static
void describeNode(const NFA *n, const sheng *s, u16 i, FILE *f) {
    const sstate_aux *aux = get_aux(n, i);

    fprintf(f, "%u [ width = 1, fixedsize = true, fontsize = 12, "
               "label = \"%u\" ]; \n",
            i, i);

    if (aux->accept_eod) {
        fprintf(f, "%u [ color = darkorchid ];\n", i);
    }

    if (aux->accept) {
        fprintf(f, "%u [ shape = doublecircle ];\n", i);
    }

    if (aux->top && (aux->top & SHENG_STATE_MASK) != i) {
        fprintf(f, "%u -> %u [color = darkgoldenrod weight=0.1 ]\n", i,
                aux->top & SHENG_STATE_MASK);
    }

    if (i == (s->anchored & SHENG_STATE_MASK)) {
        fprintf(f, "STARTA -> %u [color = blue ]\n", i);
    }

    if (i == (s->floating & SHENG_STATE_MASK)) {
        fprintf(f, "STARTF -> %u [color = red ]\n", i);
    }
}

static
void describeEdge(FILE *f, const u16 *t, u16 i) {
    for (u16 s = 0; s < N_CHARS; s++) {
        if (!t[s]) {
            continue;
        }

        u16 ss;
        for (ss = 0; ss < s; ss++) {
            if (t[s] == t[ss]) {
                break;
            }
        }

        if (ss != s) {
            continue;
        }

        CharReach reach;
        for (ss = s; ss < 256; ss++) {
            if (t[s] == t[ss]) {
                reach.set(ss);
            }
        }

        fprintf(f, "%u -> %u [ label = \"", i, t[s]);

        describeClass(f, reach, 5, CC_OUT_DOT);

        fprintf(f, "\" ];\n");
    }
}

static
void shengGetTransitions(const NFA *n, u16 state, u16 *t) {
    assert(isShengType(n->type));
    const sheng *s = (const sheng *)getImplNfa(n);
    const sstate_aux *aux = get_aux(n, state);

    for (unsigned i = 0; i < N_CHARS; i++) {
        u8 buf[16];
        m128 shuffle_mask = s->shuffle_masks[i];

        store128(buf, shuffle_mask);

        t[i] = buf[state] & SHENG_STATE_MASK;
    }

    t[TOP] = aux->top & SHENG_STATE_MASK;
}

void nfaExecSheng_dumpDot(const NFA *nfa, FILE *f, const string &) {
    assert(nfa->type == SHENG_NFA);
    const sheng *s = (const sheng *)getImplNfa(nfa);

    dumpDotPreambleDfa(f);

    for (u16 i = 1; i < s->n_states; i++) {
        describeNode(nfa, s, i, f);

        u16 t[ALPHABET_SIZE];

        shengGetTransitions(nfa, i, t);

        describeEdge(f, t, i);
    }

    fprintf(f, "}\n");
}

} // namespace ue2
