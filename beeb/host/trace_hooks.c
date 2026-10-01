// trace_hooks.c - the traced build's side of the wrappers and counters that
// host/trace.py puts into render.c and arith.c (PLAN.md D1c, D1d, 5.6).
// Included by beebtool.c right after render_traced.c, so it sees every
// static of the engine.
//
//   the unit-replay recorder   record DIR
//   its self-check             replaycheck DIR
//   the frame-cost model       cost DIR
//
// The record format is host/replay.md's; every encoder here has its decoder
// beside it, and replaycheck proves the pair complete: each record, restored
// into a fresh renderer and run again, must reproduce itself exactly.

// ---- the canonical encoding ----

typedef struct tr_buf {
    uint8_t *p;
    uint32_t n;
    uint32_t cap;
} tr_buf;

static void put8(tr_buf *b, uint32_t v)
{
    RC_PANIC(b->n < b->cap);
    b->p[b->n++] = (uint8_t)v;
}

static void put16(tr_buf *b, uint32_t v)
{
    put8(b, v);
    put8(b, v >> 8);
}

static void put32(tr_buf *b, uint32_t v)
{
    put16(b, v);
    put16(b, v >> 16);
}

static uint8_t get8(const uint8_t **q)
{
    return *(*q)++;
}

static uint16_t get16(const uint8_t **q)
{
    uint16_t v = get8(q);
    return (uint16_t)(v | get8(q) << 8);
}

static uint32_t get32(const uint8_t **q)
{
    uint32_t v = get16(q);
    return v | (uint32_t)get16(q) << 16;
}

// the camera block: the map, the pose as the frame set it up, the sprites flag
static void enc_cam(tr_buf *b, const pt_render *r, uint8_t map)
{
    put8(b, map);
    put16(b, r->cam_x);
    put16(b, r->cam_y);
    put8(b, r->eye_z);
    put8(b, r->angle);
    put8(b, r->cam_cell);
    put8(b, r->sprites);
}

static uint8_t dec_cam(const uint8_t **q, pt_render *r)
{
    uint8_t map = get8(q);
    r->cam_x = get16(q);
    r->cam_y = get16(q);
    r->eye_z = get8(q);
    r->angle = get8(q);
    r->cam_cell = get8(q);
    r->sprites = get8(q);
    return map;
}

static void enc_vv(tr_buf *b, pt_vv v)
{
    put16(b, (uint16_t)v.x);
    put16(b, (uint16_t)v.y);
    put8(b, v.sx);
    put8(b, v.flags);
    put8(b, v.rx.f);
    put8(b, v.rx.e);
}

static pt_vv dec_vv(const uint8_t **q)
{
    pt_vv v;
    v.x = (int16_t)get16(q);
    v.y = (int16_t)get16(q);
    v.sx = get8(q);
    v.flags = get8(q);
    v.rx.f = get8(q);
    v.rx.e = get8(q);
    return v;
}

static void enc_line(tr_buf *b, const pt_line *l)
{
    put8(b, l->x0);
    put8(b, l->x1);
    put8(b, l->y0);
    put16(b, l->g);
    put8(b, l->ce);
    put8(b, l->co);
}

static pt_line dec_line(const uint8_t **q)
{
    pt_line l;
    l.x0 = get8(q);
    l.x1 = get8(q);
    l.y0 = get8(q);
    l.g = get16(q);
    l.ce = get8(q);
    l.co = get8(q);
    return l;
}

static void enc_portal(tr_buf *b, const pt_portal *p)
{
    put8(b, p->cell);
    put8(b, p->x);
    put8(b, p->xr);
    put8(b, p->tcur);
    put8(b, p->bcur);
    put8(b, p->tmax);
    put8(b, p->bmin);
}

static pt_portal dec_portal(const uint8_t **q)
{
    pt_portal p;
    p.cell = get8(q);
    p.x = get8(q);
    p.xr = get8(q);
    p.tcur = get8(q);
    p.bcur = get8(q);
    p.tmax = get8(q);
    p.bmin = get8(q);
    return p;
}

static void enc_closed(tr_buf *b, const pt_closedl *cl)
{
    for (uint32_t i = 0; i < PT_MAX_CLOSED; i++)
        put8(b, cl->l[i]);
    for (uint32_t i = 0; i < PT_MAX_CLOSED; i++)
        put8(b, cl->r[i]);
    put8(b, cl->num);
}

static pt_closedl dec_closed(const uint8_t **q)
{
    pt_closedl cl;
    for (uint32_t i = 0; i < PT_MAX_CLOSED; i++)
        cl.l[i] = get8(q);
    for (uint32_t i = 0; i < PT_MAX_CLOSED; i++)
        cl.r[i] = get8(q);
    cl.num = get8(q);
    return cl;
}

// A pt_clip, its pointers resolved: the chain builder and closed list by
// content (a flag first: absent is a null pointer), the visit by index.
static void enc_cx(tr_buf *b, const pt_clip *cx)
{
    put8(b, cx->thead);
    put8(b, cx->bhead);
    put8(b, cx->tmax);
    put8(b, cx->bmin);
    put8(b, cx->role);
    put8(b, cx->build != NULL);
    pt_chainb cb = cx->build ? *cx->build : (pt_chainb) {0};
    put8(b, cb.head);
    put8(b, cb.tail);
    put8(b, cb.tmax);
    put8(b, cb.bmin);
    put8(b, cx->cbfb.e);
    put8(b, cx->cbfb.o);
    put8(b, cx->closed != NULL);
    pt_closedl cl = cx->closed ? *cx->closed : (pt_closedl) {0};
    enc_closed(b, &cl);
    put8(b, cx->vis ? (uint32_t)(cx->vis - cx->r->visits) : 0xFF);
    put8(b, cx->slot);
    put8(b, cx->tp);
    put8(b, cx->bp_set);
    put8(b, cx->bp);
}

// The replay's pt_clip, with storage for what it points at.
typedef struct tr_cx {
    pt_clip cx;
    pt_chainb build;
    pt_closedl closed;
} tr_cx;

static void dec_cx(const uint8_t **q, pt_render *r, tr_cx *t)
{
    pt_clip *cx = &t->cx;
    *cx = (pt_clip) {.r = r};
    cx->thead = get8(q);
    cx->bhead = get8(q);
    cx->tmax = get8(q);
    cx->bmin = get8(q);
    cx->role = get8(q);
    bool has_build = get8(q);
    t->build.head = get8(q);
    t->build.tail = get8(q);
    t->build.tmax = get8(q);
    t->build.bmin = get8(q);
    cx->build = has_build ? &t->build : NULL;
    cx->cbfb.e = get8(q);
    cx->cbfb.o = get8(q);
    bool has_closed = get8(q);
    t->closed = dec_closed(q);
    cx->closed = has_closed ? &t->closed : NULL;
    uint8_t vis = get8(q);
    cx->vis = vis == 0xFF ? NULL : &r->visits[vis];
    cx->slot = get8(q);
    cx->tp = get8(q);
    cx->bp_set = get8(q);
    cx->bp = get8(q);
}

// The frame state: everything the walk reads or writes that the frame
// builds, in index order, counts first.
static void enc_fs(tr_buf *b, const pt_render *r)
{
    put16(b, r->pool_num);
    for (uint32_t i = 0; i < r->pool_num; i++) {
        const pt_piece *p = &r->pool[i];
        put8(b, p->x0);
        put8(b, p->x1);
        put8(b, p->y0);
        put16(b, p->g);
        put8(b, p->ce);
        put8(b, p->co);
        put8(b, p->next);
    }
    put8(b, r->num_visits);
    for (uint32_t i = 0; i < r->num_visits; i++) {
        const pt_visit *v = &r->visits[i];
        put8(b, v->cell);
        put8(b, v->k);
        put8(b, v->xr);
        enc_vv(b, v->v0);
        enc_vv(b, v->v1);
        put8(b, v->inst);
        for (uint32_t s = 0; s < PT_NUM_SLOTS; s++)
            put8(b, v->active[s]);
    }
    for (uint32_t i = 0; i < PT_MAX_CELLS; i++)
        put8(b, r->cell_visit[i]);
    put8(b, r->num_insts);
    for (uint32_t i = 0; i < r->num_insts; i++) {
        const pt_inst *in = &r->insts[i];
        put8(b, in->sprite);
        put8(b, in->cx0);
        put8(b, in->cx1);
        put8(b, in->seg);
        put8(b, in->seg_tail);
        put8(b, in->prev);
        put8(b, in->next);
        put16(b, in->scale);
        put32(b, (uint32_t)in->left);
        put32(b, (uint32_t)in->top);
    }
    put8(b, r->order_head);
    put8(b, r->order_tail);
    put16(b, r->seg_num);
    for (uint32_t i = 0; i < r->seg_num; i++)
        put8(b, r->segs[i]);
    put8(b, r->num_pend);
    for (uint32_t i = 0; i < r->num_pend; i++)
        put8(b, r->pend[i]);
}

static void dec_fs(const uint8_t **q, pt_render *r)
{
    r->pool_num = get16(q);
    for (uint32_t i = 0; i < r->pool_num; i++) {
        pt_piece *p = &r->pool[i];
        p->x0 = get8(q);
        p->x1 = get8(q);
        p->y0 = get8(q);
        p->g = get16(q);
        p->ce = get8(q);
        p->co = get8(q);
        p->next = get8(q);
    }
    r->num_visits = get8(q);
    for (uint32_t i = 0; i < r->num_visits; i++) {
        pt_visit *v = &r->visits[i];
        v->cell = get8(q);
        v->k = get8(q);
        v->xr = get8(q);
        v->v0 = dec_vv(q);
        v->v1 = dec_vv(q);
        v->inst = get8(q);
        for (uint32_t s = 0; s < PT_NUM_SLOTS; s++)
            v->active[s] = get8(q);
    }
    for (uint32_t i = 0; i < PT_MAX_CELLS; i++)
        r->cell_visit[i] = get8(q);
    r->num_insts = get8(q);
    for (uint32_t i = 0; i < r->num_insts; i++) {
        pt_inst *in = &r->insts[i];
        *in = (pt_inst) {0};    // the sprite pass's fields are its own, zero until it runs
        in->sprite = get8(q);
        in->cx0 = get8(q);
        in->cx1 = get8(q);
        in->seg = get8(q);
        in->seg_tail = get8(q);
        in->prev = get8(q);
        in->next = get8(q);
        in->scale = get16(q);
        in->left = (int32_t)get32(q);
        in->top = (int32_t)get32(q);
    }
    r->order_head = get8(q);
    r->order_tail = get8(q);
    r->seg_num = get16(q);
    for (uint32_t i = 0; i < r->seg_num; i++)
        r->segs[i] = get8(q);
    r->num_pend = get8(q);
    for (uint32_t i = 0; i < r->num_pend; i++)
        r->pend[i] = get8(q);
}

// ---- the recorder ----
//
// Recording runs the pose streams twice. The scan pass notes, per function,
// the call numbers worth keeping: the first TR_FIRST calls, every call
// during which a basic block ran for the first time (the trace-pc-guard
// callbacks count them), and every TR_STRIDE-th call for variety, up to
// TR_CAP in all. The write pass
// renders the same poses again, deterministically the same calls, and
// records just those. The before-state is only encoded for a call about to
// be kept, which is what makes recording tens of thousands of frames cheap.

enum {
    TR_emit, TR_resolve_block, TR_clip_line, TR_clip_edge_line, TR_cut, TR_inst_new, TR_seg_record,
    TR_inst_issue, TR_child_portal, TR_next_open, TR_close_columns, TR_process_edge, TR_N,
};

static const char *const tr_names_[TR_N] = {
    "emit", "resolve_block", "clip_line", "clip_edge_line", "cut", "inst_new", "seg_record",
    "inst_issue", "child_portal", "next_open", "close_columns", "process_edge",
};

// next_open never touches the renderer: its records carry no frame state
static bool tr_has_fs(uint32_t fn)
{
    return fn != TR_next_open;
}

#define TR_FIRST 64
#define TR_STRIDE 1024
#define TR_CAP 2000
#define TR_DEPTH 16          // wrapped calls nest this deep at most
#define TR_BUF (16 * 1024)   // one record, before or after half

enum { TR_OFF, TR_SCAN, TR_WRITE };

typedef struct tr_frame {
    uint32_t fn;
    uint32_t n;         // its call number
    bool keep;
    uint32_t blocks;    // the block count at the call (scan pass)
    uint8_t buf[TR_BUF];
    tr_buf b;           // camera, arguments, frame state before
} tr_frame;

static struct {
    int mode;
    uint8_t map;
    uint32_t calls[TR_N];      // calls so far, per function
    uint32_t keep[TR_N][TR_CAP];    // the call numbers kept, ascending
    uint32_t keep_n[TR_N];
    uint32_t next[TR_N];       // write pass: the next entry of keep to meet
    uint32_t written[TR_N];
    uint32_t dropped[TR_N];    // novel calls past TR_CAP
    FILE *out[TR_N];
    tr_frame stack[TR_DEPTH];
    uint32_t depth;
} tr_;

#ifndef BEEBTOOL_SEARCH
// A coverage block count for the scan pass, from the same trace-pc-guard
// callbacks the pose search uses.
static uint32_t blocks_;

__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard_init(uint32_t *start,
                                                                                 uint32_t *stop)
{
    for (uint32_t *g = start; g < stop; g++)
        *g = 1;
}

__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    if (!*guard)
        return;
    *guard = 0;
    blocks_++;
}
#endif

// Enter a wrapped call: its frame, and whether to encode its before-state.
static tr_frame *tr_enter(uint32_t fn, const pt_render *r)
{
    RC_PANIC(tr_.depth < TR_DEPTH);
    tr_frame *f = &tr_.stack[tr_.depth++];
    f->fn = fn;
    uint32_t n = tr_.calls[fn]++;
    f->n = n;
    if (tr_.mode == TR_SCAN) {
        f->keep = false;
        f->blocks = blocks_;
        return NULL;
    }
    f->keep = tr_.mode == TR_WRITE && tr_.next[fn] < tr_.keep_n[fn] && tr_.keep[fn][tr_.next[fn]] == n;
    if (!f->keep)
        return NULL;
    tr_.next[fn]++;
    f->b = (tr_buf) {.p = f->buf, .cap = TR_BUF};
    enc_cam(&f->b, r, tr_.map);
    return f;
}

// Leave it: in the scan pass decide whether it is worth keeping; in the
// write pass return the frame to finish (after-state and results) or NULL.
static tr_frame *tr_leave(uint32_t fn)
{
    tr_frame *f = &tr_.stack[--tr_.depth];
    RC_ASSERT(f->fn == fn);
    if (tr_.mode == TR_SCAN)
        return f;    // the hook decides, straight away (tr_scan_decide)
    return f->keep ? f : NULL;
}

// The scan pass's verdict on a call, once it has returned.
static void tr_scan_decide(tr_frame *f)
{
    bool novel = blocks_ != f->blocks;
    if (f->n < TR_FIRST || novel || f->n % TR_STRIDE == 0) {
        if (tr_.keep_n[f->fn] < TR_CAP)
            tr_.keep[f->fn][tr_.keep_n[f->fn]++] = f->n;
        else
            tr_.dropped[f->fn]++;
    }
}

static void tr_write(tr_frame *f, const uint8_t *after, uint32_t after_n)
{
    FILE *o = tr_.out[f->fn];
    uint32_t len = f->b.n + after_n;
    RC_PANIC(len < 65536);
    uint8_t hdr[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
    fwrite(hdr, 1, 2, o);
    fwrite(f->b.p, 1, f->b.n, o);
    fwrite(after, 1, after_n, o);
    tr_.written[f->fn]++;
}

// The shape every hook pair shares: before encodes the arguments and the
// frame state; after encodes the frame state and the results, and writes.
#define TR_BEFORE(fn, r)                                     \
    tr_frame *f = tr_.mode == TR_OFF ? NULL : tr_enter(TR_##fn, r); \
    if (!f)                                                  \
        return;                                              \
    tr_buf *b = &f->b;

#define TR_AFTER(fn, r)                                      \
    if (tr_.mode == TR_OFF)                                  \
        return;                                              \
    tr_frame *f = tr_leave(TR_##fn);                         \
    if (tr_.mode == TR_SCAN) {                               \
        tr_scan_decide(f);                                   \
        return;                                              \
    }                                                        \
    if (!f)                                                  \
        return;                                              \
    static uint8_t abuf_[TR_BUF];                            \
    tr_buf ab_ = {.p = abuf_, .cap = TR_BUF};                \
    tr_buf *b = &ab_;                                        \
    if (tr_has_fs(TR_##fn))                                  \
        enc_fs(b, r);

#define TR_DONE() tr_write(f, ab_.p, ab_.n)

static void tr_before_emit(pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1, uint8_t st)
{
    TR_BEFORE(emit, cx->r);
    enc_cx(b, cx);
    enc_line(b, L);
    put8(b, r0);
    put8(b, r1);
    put8(b, st);
    enc_fs(b, cx->r);
}

static void tr_after_emit(pt_clip *cx, const pt_line *L, uint8_t r0, uint8_t r1, uint8_t st)
{
    (void) L, (void) r0, (void) r1, (void) st;
    TR_AFTER(emit, cx->r);
    enc_cx(b, cx);
    TR_DONE();
}

static void tr_before_resolve_block(pt_clip *cx, const pt_line *L, uint8_t c, uint8_t e)
{
    TR_BEFORE(resolve_block, cx->r);
    enc_cx(b, cx);
    enc_line(b, L);
    put8(b, c);
    put8(b, e);
    enc_fs(b, cx->r);
}

static void tr_after_resolve_block(pt_clip *cx, const pt_line *L, uint8_t c, uint8_t e)
{
    (void) L, (void) c, (void) e;
    TR_AFTER(resolve_block, cx->r);
    enc_cx(b, cx);
    TR_DONE();
}

static void tr_before_clip_line(pt_clip *cx, const pt_line *L, uint8_t st)
{
    TR_BEFORE(clip_line, cx->r);
    enc_cx(b, cx);
    enc_line(b, L);
    put8(b, st);
    enc_fs(b, cx->r);
}

static void tr_after_clip_line(pt_clip *cx, const pt_line *L, uint8_t st)
{
    (void) L, (void) st;
    TR_AFTER(clip_line, cx->r);
    enc_cx(b, cx);
    TR_DONE();
}

static void tr_before_clip_edge_line(pt_clip *cx, pt_line L, pt_relh h, uint8_t xa, uint8_t xb)
{
    TR_BEFORE(clip_edge_line, cx->r);
    enc_cx(b, cx);
    enc_line(b, &L);
    put8(b, h.m);
    put8(b, h.below);
    put8(b, xa);
    put8(b, xb);
    enc_fs(b, cx->r);
}

static void tr_after_clip_edge_line(pt_clip *cx, pt_line L, pt_relh h, uint8_t xa, uint8_t xb)
{
    (void) L, (void) h, (void) xa, (void) xb;
    TR_AFTER(clip_edge_line, cx->r);
    enc_cx(b, cx);
    TR_DONE();
}

static void tr_before_cut(pt_render *r, uint8_t *cursor, uint8_t xa, uint8_t xb)
{
    TR_BEFORE(cut, r);
    put8(b, *cursor);
    put8(b, xa);
    put8(b, xb);
    enc_fs(b, r);
}

static void tr_after_cut(pt_render *r, uint8_t *cursor, uint8_t xa, uint8_t xb, uint8_t ret)
{
    (void) xa, (void) xb;
    TR_AFTER(cut, r);
    put8(b, ret);
    put8(b, *cursor);
    TR_DONE();
}

static void tr_before_inst_new(pt_render *r, uint8_t cell)
{
    TR_BEFORE(inst_new, r);
    put8(b, cell);
    enc_fs(b, r);
}

static void tr_after_inst_new(pt_render *r, uint8_t cell, uint8_t ret)
{
    (void) cell;
    TR_AFTER(inst_new, r);
    put8(b, ret);
    TR_DONE();
}

static void tr_before_seg_record(pt_render *r, const pt_portal *p)
{
    TR_BEFORE(seg_record, r);
    enc_portal(b, p);
    enc_fs(b, r);
}

static void tr_after_seg_record(pt_render *r, const pt_portal *p)
{
    (void) p;
    TR_AFTER(seg_record, r);
    TR_DONE();
}

static void tr_before_inst_issue(pt_render *r, uint8_t pend)
{
    TR_BEFORE(inst_issue, r);
    put8(b, pend);
    enc_fs(b, r);
}

static void tr_after_inst_issue(pt_render *r, uint8_t pend)
{
    (void) pend;
    TR_AFTER(inst_issue, r);
    TR_DONE();
}

static void tr_before_child_portal(pt_render *r, uint8_t ei, uint8_t xl, uint8_t xr, uint8_t thead,
                                   uint8_t bhead, uint8_t tmax, uint8_t bmin, pt_vv pv, pt_portal *out)
{
    (void) out;
    TR_BEFORE(child_portal, r);
    put8(b, ei);
    put8(b, xl);
    put8(b, xr);
    put8(b, thead);
    put8(b, bhead);
    put8(b, tmax);
    put8(b, bmin);
    enc_vv(b, pv);
    enc_fs(b, r);
}

static void tr_after_child_portal(pt_render *r, uint8_t ei, uint8_t xl, uint8_t xr, uint8_t thead,
                                  uint8_t bhead, uint8_t tmax, uint8_t bmin, pt_vv pv, pt_portal *out,
                                  bool ret)
{
    (void) ei, (void) xl, (void) xr, (void) thead, (void) bhead, (void) tmax, (void) bmin, (void) pv;
    TR_AFTER(child_portal, r);
    put8(b, ret);
    pt_portal none = {0};
    enc_portal(b, ret ? out : &none);    // defined only when there is a child
    TR_DONE();
}

static void tr_before_next_open(const pt_closedl *cl, uint8_t *c, uint8_t xb, uint8_t *ol, uint8_t *orr)
{
    (void) ol, (void) orr;
    TR_BEFORE(next_open, &render_);    // no renderer of its own: the frame's, for the camera
    enc_closed(b, cl);
    put8(b, *c);
    put8(b, xb);
}

static void tr_after_next_open(const pt_closedl *cl, uint8_t *c, uint8_t xb, uint8_t *ol, uint8_t *orr,
                               bool ret)
{
    (void) cl, (void) xb;
    TR_AFTER(next_open, &render_);
    put8(b, ret);
    put8(b, *c);
    put8(b, ret ? *ol : 0);     // defined only when there is a range
    put8(b, ret ? *orr : 0);
    TR_DONE();
}

static void tr_before_close_columns(pt_render *r, uint8_t head, uint8_t l, uint8_t rr, pt_pattern cbfb)
{
    TR_BEFORE(close_columns, r);
    put8(b, head);
    put8(b, l);
    put8(b, rr);
    put8(b, cbfb.e);
    put8(b, cbfb.o);
    enc_fs(b, r);
}

static void tr_after_close_columns(pt_render *r, uint8_t head, uint8_t l, uint8_t rr, pt_pattern cbfb)
{
    (void) head, (void) l, (void) rr, (void) cbfb;
    TR_AFTER(close_columns, r);
    TR_DONE();
}

static void tr_before_process_edge(pt_render *r, pt_portal *p, pt_portal *child)
{
    (void) child;
    TR_BEFORE(process_edge, r);
    enc_portal(b, p);
    enc_fs(b, r);
}

static void tr_after_process_edge(pt_render *r, pt_portal *p, pt_portal *child, bool ret)
{
    TR_AFTER(process_edge, r);
    put8(b, ret);
    enc_portal(b, p);
    pt_portal none = {0};
    enc_portal(b, ret ? child : &none);
    TR_DONE();
}

// ---- running the streams ----

// The golden streams the recorder and the cost model walk, in order.
typedef struct tr_stream {
    int map;
    const char *file;
} tr_stream;

static const tr_stream tr_streams_[] = {
    {0, "sweep_poses.bin"},  {7, "map7_poses.bin"},    {0, "rim_poses.bin"},     {9, "stress_poses.bin"},
    {0, "search0_poses.bin"}, {7, "search7_poses.bin"}, {9, "search9_poses.bin"},
};

// Render every stream's poses, calling each(frame index) after each frame.
static void tr_run(const char *dir, void (*each)(uint32_t frame, int map))
{
    char path[1024];
    uint32_t frame = 0;
    for (uint32_t s = 0; s < sizeof tr_streams_ / sizeof tr_streams_[0]; s++) {
        snprintf(path, sizeof path, "%s/%s", dir, tr_streams_[s].file);
        FILE *probe = fopen(path, "rb");
        if (!probe)
            continue;    // the search streams are optional
        fclose(probe);
        load_map(tr_streams_[s].map);
        poison(0);
        pt_render_init(&render_, &map_);
        tr_.map = (uint8_t)tr_streams_[s].map;
        size_t n;
        uint8_t *p = read_file(path, &n);
        for (size_t i = 0; i + 6 <= n; i += 6, frame++) {
            memset(tc_, 0, sizeof tc_);
            pt_render_frame(&render_, (uint16_t)(p[i] | p[i + 1] << 8), (uint16_t)(p[i + 2] | p[i + 3] << 8),
                            p[i + 4], p[i + 5]);
            if (each)
                each(frame, tr_streams_[s].map);
        }
        free(p);
    }
}

static void record(const char *dir)
{
    tr_.mode = TR_SCAN;
    tr_run(dir, NULL);

    char path[1024];
    snprintf(path, sizeof path, "%s/replay", dir);
    mkdir(path, 0777);
    for (uint32_t fn = 0; fn < TR_N; fn++) {
        snprintf(path, sizeof path, "%s/replay/%s.bin", dir, tr_names_[fn]);
        tr_.out[fn] = open_out(NULL, path);
        uint8_t hdr[8] = {'P', 'C', 'R', 'P', 1, (uint8_t)fn, 0, 0};
        fwrite(hdr, 1, sizeof hdr, tr_.out[fn]);
        tr_.calls[fn] = 0;
        tr_.next[fn] = 0;
    }
    tr_.mode = TR_WRITE;
    tr_run(dir, NULL);
    tr_.mode = TR_OFF;
    for (uint32_t fn = 0; fn < TR_N; fn++) {
        fclose(tr_.out[fn]);
        printf("%-15s %7u calls, %4u records%s\n", tr_names_[fn], tr_.calls[fn], tr_.written[fn],
               tr_.dropped[fn] ? " (novel calls past the cap dropped)" : "");
    }
}

// ---- the self-check ----

// Replay one record: restore it into a fresh renderer, run the function
// (the __impl, so nothing records), encode the after half the way the
// recorder did, and compare.
static bool replay_one(uint32_t fn, const uint8_t *rec, uint32_t len, tr_buf *got)
{
    static pt_render r;
    static int loaded = -1;
    const uint8_t *q = rec;
    if (rec[0] != loaded) {
        loaded = rec[0];
        load_map(loaded);
    }
    pt_render_init(&r, &map_);
    dec_cam(&q, &r);

    tr_cx t;
    pt_portal p, child = {0}, out = {0};
    uint8_t cursor, ol = 0, orr = 0, c;
    pt_closedl cl;
    got->n = 0;
    switch (fn) {
    case TR_emit: {
        dec_cx(&q, &r, &t);
        pt_line L = dec_line(&q);
        uint8_t r0 = get8(&q), r1 = get8(&q), st = get8(&q);
        dec_fs(&q, &r);
        emit__impl(&t.cx, &L, r0, r1, st);
        enc_fs(got, &r);
        enc_cx(got, &t.cx);
        break;
    }
    case TR_resolve_block: {
        dec_cx(&q, &r, &t);
        pt_line L = dec_line(&q);
        uint8_t c0 = get8(&q), e = get8(&q);
        dec_fs(&q, &r);
        resolve_block__impl(&t.cx, &L, c0, e);
        enc_fs(got, &r);
        enc_cx(got, &t.cx);
        break;
    }
    case TR_clip_line: {
        dec_cx(&q, &r, &t);
        pt_line L = dec_line(&q);
        uint8_t st = get8(&q);
        dec_fs(&q, &r);
        clip_line__impl(&t.cx, &L, st);
        enc_fs(got, &r);
        enc_cx(got, &t.cx);
        break;
    }
    case TR_clip_edge_line: {
        dec_cx(&q, &r, &t);
        pt_line L = dec_line(&q);
        pt_relh h;
        h.m = get8(&q);
        h.below = get8(&q);
        uint8_t xa = get8(&q), xb = get8(&q);
        dec_fs(&q, &r);
        clip_edge_line__impl(&t.cx, L, h, xa, xb);
        enc_fs(got, &r);
        enc_cx(got, &t.cx);
        break;
    }
    case TR_cut: {
        cursor = get8(&q);
        uint8_t xa = get8(&q), xb = get8(&q);
        dec_fs(&q, &r);
        uint8_t ret = cut__impl(&r, &cursor, xa, xb);
        enc_fs(got, &r);
        put8(got, ret);
        put8(got, cursor);
        break;
    }
    case TR_inst_new: {
        uint8_t cell = get8(&q);
        dec_fs(&q, &r);
        uint8_t ret = inst_new__impl(&r, cell);
        enc_fs(got, &r);
        put8(got, ret);
        break;
    }
    case TR_seg_record:
        p = dec_portal(&q);
        dec_fs(&q, &r);
        seg_record__impl(&r, &p);
        enc_fs(got, &r);
        break;
    case TR_inst_issue: {
        uint8_t pend = get8(&q);
        dec_fs(&q, &r);
        inst_issue__impl(&r, pend);
        enc_fs(got, &r);
        break;
    }
    case TR_child_portal: {
        uint8_t ei = get8(&q), xl = get8(&q), xr = get8(&q), th = get8(&q), bh = get8(&q);
        uint8_t tmax = get8(&q), bmin = get8(&q);
        pt_vv pv = dec_vv(&q);
        dec_fs(&q, &r);
        bool ret = child_portal__impl(&r, ei, xl, xr, th, bh, tmax, bmin, pv, &out);
        enc_fs(got, &r);
        put8(got, ret);
        pt_portal none = {0};
        enc_portal(got, ret ? &out : &none);
        break;
    }
    case TR_next_open: {
        cl = dec_closed(&q);
        c = get8(&q);
        uint8_t xb = get8(&q);
        bool ret = next_open__impl(&cl, &c, xb, &ol, &orr);
        put8(got, ret);
        put8(got, c);
        put8(got, ret ? ol : 0);
        put8(got, ret ? orr : 0);
        break;
    }
    case TR_close_columns: {
        uint8_t head = get8(&q), l = get8(&q), rr = get8(&q);
        pt_pattern cbfb;
        cbfb.e = get8(&q);
        cbfb.o = get8(&q);
        dec_fs(&q, &r);
        close_columns__impl(&r, head, l, rr, cbfb);
        enc_fs(got, &r);
        break;
    }
    case TR_process_edge: {
        p = dec_portal(&q);
        dec_fs(&q, &r);
        bool ret = process_edge__impl(&r, &p, &child);
        enc_fs(got, &r);
        put8(got, ret);
        enc_portal(got, &p);
        pt_portal none = {0};
        enc_portal(got, ret ? &child : &none);
        break;
    }
    default:
        RC_PANIC(false);
    }
    uint32_t used = (uint32_t)(q - rec);
    return len - used == got->n && memcmp(q, got->p, got->n) == 0;
}

static void replaycheck(const char *dir)
{
    static uint8_t gbuf[TR_BUF];
    tr_buf got = {.p = gbuf, .cap = TR_BUF};
    char path[1024];
    uint32_t total = 0;
    uint32_t bad = 0;
    for (uint32_t fn = 0; fn < TR_N; fn++) {
        snprintf(path, sizeof path, "%s/replay/%s.bin", dir, tr_names_[fn]);
        size_t n;
        uint8_t *buf = read_file(path, &n);
        RC_PANIC(n >= 8 && memcmp(buf, "PCRP", 4) == 0 && buf[4] == 1 && buf[5] == fn);
        uint32_t recs = 0;
        uint32_t fails = 0;
        for (size_t i = 8; i + 2 <= n;) {
            uint32_t len = (uint32_t)(buf[i] | buf[i + 1] << 8);
            RC_PANIC(i + 2 + len <= n);
            if (!replay_one(fn, buf + i + 2, len, &got))
                fails++;
            recs++;
            i += 2 + len;
        }
        printf("%-15s %4u records: %s\n", tr_names_[fn], recs,
               fails ? "MISMATCHES" : "each one reproduces itself");
        if (fails)
            printf("                %u of them differ\n", fails);
        total += recs;
        bad += fails;
        free(buf);
    }
    printf("%u records, %s\n", total, bad ? "FAIL" : "all reproduce");
    if (bad)
        exit(1);
}

// ---- the cost model (D1d) ----
//
// Each counter times a 6502 cycle estimate, per frame: the Baron sketches'
// figures where there are any, guesses otherwise, to be re-weighted with
// the cycles measured in Phases 2-4 (PLAN.md 5.9). Fixed costs (clear,
// fill, the swap, the camera) are not counted here.

static double tc_weight(uint32_t i)
{
    switch (i) {
    case TC_transform: return 90;          // offsets, flags, beyond the rotation
    case TC_rot: return 300;               // four 16x8 products and sums
    case TC_recip16: return 45;
    case TC_recip8: return 25;
    case TC_project_col: return 140;
    case TC_project_rowd: return 85;
    case TC_project_q8: return 140;
    case TC_grad_mag: return 75;           // beyond its recip8
    case TC_gradq8_mag: return 140;
    case TC_div_t: return 130;
    case TC_div_cols: return 140;
    case TC_mulg: return 70;
    case TC_piece_y: return 25;            // beyond the mulg
    case TC_line_y: return 25;
    case TC_process_edge: return 60;
    case TC_edge_work: return 250;
    case TC_make_line: return 120;
    case TC_near_clip: return 250;
    case TC_line_reuse: return 40;
    case TC_clip_open: return 90;
    case TC_clip_forced: return 120;
    case TC_clip_fast: return 0;           // inside clip_open's figure
    case TC_resolve_block: return 300;
    case TC_crossing: return 40;           // beyond its div_cols
    case TC_emit_extend: return 60;
    case TC_emit_visible: return 130;
    case TC_emit_merge: return 140;
    case TC_pool_split: return 90;
    case TC_cut: return 70;
    case TC_child_portal: return 160;
    case TC_walk_entry: return 120;
    case TC_plot_pieces: return 110;       // colour test, set-up, cursor from the row tables
    case TC_plot_flat_pieces: return 90;
    case TC_plot_flat_bytes: return 48;
    case TC_plot_sh_bytes: return 107;
    case TC_plot_sh_cap_px: return 50;
    case TC_plot_steep_cols: return 60;
    case TC_spr_bytecols: return 150;
    case TC_spr_rows_scanned: return 11;
    case TC_spr_pixcols: return 150;
    case TC_spr_paints: return 120;
    case TC_spr_puts: return 45;
    case TC_spr_erases: return 45;
    default: return 0;
    }
}

// the stage each counter belongs to: 0 capture, 1 plot, 2 sprites
static int tc_stage(uint32_t i)
{
    return i >= TC_spr_bytecols ? 2 : i >= TC_plot_pieces ? 1 : 0;
}

#define TC_MAX_FRAMES 100000
static double cost_[TC_MAX_FRAMES][4];    // capture, plot, sprites, total
static int cost_map_[TC_MAX_FRAMES];
static unsigned long tc_tot_[TC_N], tc_max_[TC_N];
static uint32_t cost_frames_;

static void cost_frame(uint32_t frame, int map)
{
    RC_PANIC(frame < TC_MAX_FRAMES);
    double s[3] = {0, 0, 0};
    for (uint32_t i = 0; i < TC_N; i++) {
        s[tc_stage(i)] += tc_weight(i) * (double)tc_[i];
        tc_tot_[i] += tc_[i];
        if (tc_[i] > tc_max_[i])
            tc_max_[i] = tc_[i];
    }
    cost_[frame][0] = s[0];
    cost_[frame][1] = s[1];
    cost_[frame][2] = s[2];
    cost_[frame][3] = s[0] + s[1] + s[2];
    cost_map_[frame] = map;
    cost_frames_ = frame + 1;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void cost(const char *dir)
{
    tr_run(dir, cost_frame);
    static double sorted[TC_MAX_FRAMES];
    static const int maps[] = {0, 7, 9};
    printf("variable work per frame, estimated 6502 cycles (capture + plot + sprites; clear, fill,\n"
           "swap and camera not included)\n\n");
    printf("map  frames      mean   capture      plot   sprites    median       90%%       max\n");
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t n = 0;
        double m[4] = {0, 0, 0, 0};
        for (uint32_t f = 0; f < cost_frames_; f++) {
            if (cost_map_[f] != maps[k])
                continue;
            for (uint32_t j = 0; j < 4; j++)
                m[j] += cost_[f][j];
            sorted[n++] = cost_[f][3];
        }
        if (n == 0)
            continue;
        qsort(sorted, n, sizeof sorted[0], cmp_double);
        printf("%3d %7u %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f\n", maps[k], n, m[3] / n, m[0] / n, m[1] / n,
               m[2] / n, sorted[n / 2], sorted[n * 9 / 10], sorted[n - 1]);
    }
    printf("\ncounter            mean/frame   max\n");
    for (uint32_t i = 0; i < TC_N; i++)
        printf("%-18s %10.1f %6lu\n", tc_names_[i], (double)tc_tot_[i] / cost_frames_, tc_max_[i]);
}
