#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

// Integers
// --------

typedef uint8_t bool;

typedef uint8_t u8;
typedef uint16_t u16; 
typedef uint32_t u32;
typedef uint64_t u64;

typedef _Atomic(u8) a8;
typedef _Atomic(u16) a16;
typedef _Atomic(u32) a32; 
typedef _Atomic(u64) a64;

// Configuration
// -------------

// Threads per CPU
const u32 TPC_L2 = 3;
const u32 TPC    = 1 << TPC_L2;

// Program
const u32 DEPTH = 10;
const u32 LOOPS = 65536;

// Types
// -----

// Local Types
typedef u8  Tag;  // Tag  ::= 3-bit (rounded up to u8)
typedef u32 Val;  // Val  ::= 29-bit (rounded up to u32)
typedef u32 Port; // Port ::= Tag + Val (fits a u32)
typedef u64 Pair; // Pair ::= Port + Port (fits a u64)

typedef a32 APort; // atomic Port
typedef a64 APair; // atomic Pair

// Rules
typedef u8 Rule; // Rule ::= 3-bit (rounded up to 8)

// Tags
const Tag VAR = 0x0; // variable
const Tag REF = 0x1; // reference
const Tag ERA = 0x2; // eraser
const Tag NUM = 0x3; // number
const Tag CON = 0x4; // constructor
const Tag DUP = 0x5; // duplicator
const Tag OPR = 0x6; // operator
const Tag SWI = 0x7; // switch

// Interaction Rule Values
const Rule LINK = 0x0;
const Rule CALL = 0x1;
const Rule VOID = 0x2;
const Rule ERAS = 0x3;
const Rule ANNI = 0x4;
const Rule COMM = 0x5;
const Rule OPER = 0x6;
const Rule SWIT = 0x7;

// Thread Redex Bag Length  
const u32 RLEN = 1 << 22; // max 4m redexes

// Thread Redex Bag
typedef struct RBag {
  u32  lo_idx; // high-priority stack push-index
  u32  hi_idx; // low-priority stack push-index
  Pair buf[RLEN]; // a buffer for both stacks
} RBag;

// Global Net  
const u32 G_NODE_LEN = 1 << 29; // max 536m nodes 
const u32 G_VARS_LEN = 1 << 29; // max 536m vars 
const u32 G_RBAG_LEN = TPC * RLEN;

typedef struct GNet {
  APair node_buf[G_NODE_LEN]; // global node buffer
  APort vars_buf[G_VARS_LEN]; // global vars buffer
  APair steal[TPC/2]; // steal buffer
  a64 itrs; // interaction count
} GNet;

typedef struct TMem {
  u32  tid; // thread id
  u32  tick; // tick counter
  u32  page; // page index
  u32  itrs; // interaction count
  u32  nidx; // next node allocation attempt index
  u32  vidx; // next vars allocation attempt index
  u32  node_loc[32]; // node allocation indices
  u32  vars_loc[32]; // vars allocation indices
  RBag rbag; // local bag
} TMem;  

// Top-Level Definition
typedef struct Def {
  u32  rbag_len;
  Pair rbag_buf[32];
  u32  node_len; 
  Pair node_buf[32];
  u32  vars_len;
} Def;

// Book of Definitions
typedef struct Book {
  u32 defs_len;
  Def defs_buf[64];
} Book;

// Booleans
const bool true  = 1;
const bool false = 0;

// good job, that compiles. implement the rest of the code now.

// Debugger
// --------

typedef struct {
  char x[13];
} Show;

void put_u16(char* B, u16 val);
Show show_port(Port port);
Show show_rule(Rule rule);
void print_rbag(RBag* rbag);
void print_net(GNet* net);
void pretty_print_port(GNet* net, Port port);
void pretty_print_rbag(GNet* net, RBag* rbag);

// Port: Constructor and Getters
// -----------------------------

static inline Port new_port(Tag tag, Val val) {
  return (val << 3) | tag;
}

static inline Tag get_tag(Port port) {
  return port & 7;
}

static inline Val get_val(Port port) {
  return port >> 3;
}

// Pair: Constructor and Getters
// -----------------------------

static inline const Pair new_pair(Port fst, Port snd) {
  return ((u64)snd << 32) | fst;
}

static inline Port get_fst(Pair pair) {
  return pair & 0xFFFFFFFF;
}

static inline Port get_snd(Pair pair) {
  return pair >> 32;
}

// Utils
// -----

// Swaps two ports.
static inline void swap(Port *a, Port *b) {
  Port x = *a; *a = *b; *b = x;
}

// ID of peer to share redex with.
static inline u32 peer_id(u32 id, u32 log2_len, u32 tick) {
  u32 side = (id >> (log2_len - 1 - (tick % log2_len))) & 1;
  u32 diff = (1 << (log2_len - 1)) >> (tick % log2_len);
  return side ? id - diff : id + diff;
}

// Index on the steal redex buffer for this peer pair.
static inline u32 buck_id(u32 id, u32 log2_len, u32 tick) {
  u32 fid = peer_id(id, log2_len, tick);
  u32 itv = log2_len - (tick % log2_len);
  u32 val = (id >> itv) << (itv - 1);
  return (id < fid ? id : fid) - val;
}

// Ports / Pairs / Rules
// ---------------------

// True if this port has a pointer to a node.
static inline bool is_nod(Port a) {
  return get_tag(a) >= CON;
}

// True if this port is a variable.
static inline bool is_var(Port a) {
  return get_tag(a) == VAR;
}

// Given two tags, gets their interaction rule.
static inline Rule get_rule(Port a, Port b) {
  const u8 table[8][8] = {
    //VAR  REF  ERA  NUM  CON  DUP  OPR  SWI
    {LINK,LINK,LINK,LINK,LINK,LINK,LINK,LINK}, // VAR
    {LINK,VOID,VOID,VOID,CALL,CALL,CALL,CALL}, // REF
    {LINK,VOID,VOID,VOID,ERAS,ERAS,ERAS,ERAS}, // ERA
    {LINK,VOID,VOID,VOID,ERAS,ERAS,OPER,SWIT}, // NUM
    {LINK,CALL,ERAS,ERAS,ANNI,COMM,COMM,COMM}, // CON 
    {LINK,CALL,ERAS,ERAS,COMM,ANNI,COMM,COMM}, // DUP
    {LINK,CALL,ERAS,OPER,COMM,COMM,ANNI,COMM}, // OPR
    {LINK,CALL,ERAS,SWIT,COMM,COMM,COMM,ANNI}, // SWI
  };
  return table[get_tag(a)][get_tag(b)];
}

// Same as above, but receiving a pair.
static inline Rule get_pair_rule(Pair AB) {
  return get_rule(get_fst(AB), get_snd(AB));
}

// Should we swap ports A and B before reducing this rule?
static inline bool should_swap(Port A, Port B) {
  return get_tag(B) < get_tag(A);
}

// Gets a rule's priority
static inline bool is_high_priority(Rule rule) {
  return (bool)((0b00011101 >> rule) & 1);
}

// Adjusts a newly allocated port.
static inline Port adjust_port(GNet* net, TMem* tm, Port port) {
  Tag tag = get_tag(port);
  Val val = get_val(port);
  if (is_nod(port)) return new_port(tag, tm->node_loc[val-1]);
  if (is_var(port)) return new_port(tag, tm->vars_loc[val]);
  return new_port(tag, val);
}

// Adjusts a newly allocated pair.
static inline Pair adjust_pair(GNet* net, TMem* tm, Pair pair) {
  Port p1 = adjust_port(net, tm, get_fst(pair));
  Port p2 = adjust_port(net, tm, get_snd(pair));
  return new_pair(p1, p2);
}

// RBag
// ----

void rbag_init(RBag* rbag) {
  rbag->lo_idx = 0;
  rbag->hi_idx = RLEN - 1;
}

// Keep going

static inline void push_redex(TMem* tm, Pair redex) {
  Rule rule = get_pair_rule(redex);
  if (is_high_priority(rule)) {
    tm->rbag.buf[tm->rbag.hi_idx--] = redex;
  } else {
    tm->rbag.buf[tm->rbag.lo_idx++] = redex;
  }
}

static inline Pair pop_redex(TMem* tm) {
  if (tm->rbag.hi_idx < RLEN - 1) {
    return tm->rbag.buf[++tm->rbag.hi_idx];
  } else if (tm->rbag.lo_idx > 0) {
    return tm->rbag.buf[--tm->rbag.lo_idx];
  } else {
    return 0;
  }
}

static inline u32 rbag_len(RBag* rbag) {
  return rbag->lo_idx + (RLEN - 1 - rbag->hi_idx);
}

static inline u32 rbag_has_highs(RBag* rbag) {
  return rbag->hi_idx < RLEN-1;
}

// TMem
// ----

void tmem_init(TMem* tm, u32 tid) {
  rbag_init(&tm->rbag);
  tm->tid  = tid;
  tm->tick = 0;
  tm->nidx = tid;
  tm->vidx = tid;
  tm->itrs = 0;
}

// GNet
// ----

// Stores a new node on global.
static inline void node_create(GNet* net, u32 loc, Pair val) {
  atomic_store_explicit(&net->node_buf[loc], val, memory_order_relaxed);
}

// Stores a var on global. Returns old.
static inline void vars_create(GNet* net, u32 var, Port val) {
  atomic_store_explicit(&net->vars_buf[var], val, memory_order_relaxed);
}

// Reads a node from global.
static inline Pair node_load(GNet* net, u32 loc) {
  return atomic_load_explicit(&net->node_buf[loc], memory_order_relaxed);
}

// Reads a var from global.
static inline Port vars_load(GNet* net, u32 var) {
  return atomic_load_explicit(&net->vars_buf[var], memory_order_relaxed);  
}

// Stores a node on global.
static inline void node_store(GNet* net, u32 loc, Pair val) {
  atomic_store_explicit(&net->node_buf[loc], val, memory_order_relaxed);
}

// Stores a var on global. Returns old.
static inline void vars_store(GNet* net, u32 var, Port val) {
  atomic_store_explicit(&net->vars_buf[var], val, memory_order_relaxed);
}

// Exchanges a node on global by a value. Returns old.
static inline Pair node_exchange(GNet* net, u32 loc, Pair val) {  
  return atomic_exchange_explicit(&net->node_buf[loc], val, memory_order_relaxed);
}

// Exchanges a var on global by a value. Returns old.
static inline Port vars_exchange(GNet* net, u32 var, Port val) {
  return atomic_exchange_explicit(&net->vars_buf[var], val, memory_order_relaxed);
}

// Takes a node.
static inline Pair node_take(GNet* net, u32 loc) {
  return node_exchange(net, loc, 0);
}

// Takes a var.
static inline Port vars_take(GNet* net, u32 var) {
  return vars_exchange(net, var, 0);
}

// Is a node free?
static inline bool is_node_free(GNet* net, u32 loc) {
  return node_load(net, loc) == 0;
}

// Takes a var.
static inline bool is_vars_free(GNet* net, u32 var) {
  return vars_load(net, var) == 0;
}

// Allocator
// ---------

// Allocs on node buffer. Returns the number of successful allocs.
static inline u32 node_alloc(GNet* net, TMem* tm, u32 num) {
  u32* idx = &tm->nidx;
  u32* loc = tm->node_loc;
  u32  len = G_NODE_LEN;
  u32  got = 0;
  for (u32 i = 0; i < len && got < num; ++i) {
    *idx += 1;
    if (*idx < len || is_node_free(net, *idx % len)) {
      tm->node_loc[got++] = *idx % len;
      //printf("ALLOC NODE %d %d\n", got, *idx);
    }
  }
  return got;
}

// Allocs on vars buffer. Returns the number of successful allocs.
static inline u32 vars_alloc(GNet* net, TMem* tm, u32 num) {
  u32* idx = &tm->vidx;
  u32* loc = tm->vars_loc;
  u32  len = G_VARS_LEN;
  u32  got = 0;
  for (u32 i = 0; i < len && got < num; ++i) {
    *idx += 1;
    if (*idx < len || is_vars_free(net, *idx % len)) {
      loc[got++] = *idx % len;
      //printf("ALLOC VARS %d %d\n", got, *idx);
    }
  }
  return got;
}

// Gets the necessary resources for an interaction. Returns success.
static inline bool get_resources(GNet* net, TMem* tm, u8 need_rbag, u8 need_node, u8 need_vars) {
  u32 got_rbag = RLEN - rbag_len(&tm->rbag);
  u32 got_node = node_alloc(net, tm, need_node); 
  u32 got_vars = vars_alloc(net, tm, need_vars);
  return got_rbag >= need_rbag
      && got_node >= need_node
      && got_vars >= need_vars;
}

// Linking
// -------
 
// Atomically Links `A ~ B`.
static inline void link(GNet* net, TMem* tm, Port A, Port B) {
  //printf("LINK %s ~> %s\n", show_port(A).x, show_port(B).x);

  // Attempts to directionally point `A ~> B` 
  while (true) {
    // If `A` is PRI: swap `A` and `B`, and continue
    if (get_tag(A) != VAR) {
      Port X = A; A = B; B = X;
    }
    
    // If `A` is PRI: create the `A ~ B` redex
    if (get_tag(A) != VAR) {
      push_redex(tm, new_pair(A, B)); // TODO: move global ports to local
      break;
    }

    // While `B` is VAR: extend it (as an optimization)
    while (get_tag(B) == VAR) {
      // Takes the current `B` substitution as `B'`
      Port B_ = vars_exchange(net, get_val(B), B);
      // If there was no `B'`, stop, as there is no extension
      if (B_ == B || B_ == 0) {
        break;
      }
      // Otherwise, delete `B` (we own both) and continue as `A ~> B'`
      vars_take(net, get_val(B));
      B = B_;
    }

    // Since `A` is VAR: point `A ~> B`.  
    if (true) {
      // Stores `A -> B`, taking the current `A` subst as `A'`
      Port A_ = vars_exchange(net, get_val(A), B);
      // If there was no `A'`, stop, as we lost B's ownership
      if (A_ == A) {
        break;
      }
      //if (A_ == 0) { ??? } // FIXME: must handle on the move-to-global algo
      // Otherwise, delete `A` (we own both) and link `A' ~ B`
      vars_take(net, get_val(A));
      A = A_;  
    }
  }
}

// Links `A ~ B` (as a pair).
static inline void link_pair(GNet* net, TMem* tm, Pair AB) {
  //printf("link_pair %016llx\n", AB);
  link(net, tm, get_fst(AB), get_snd(AB));
}

// Sharing
// -------

// Sends redex to a friend local thread, when it is starving.
// TODO: implement this function. Since we do not have a barrier, we must do it
// by using atomics instead. Use atomics to send data. Use busy waiting to
// receive data. Implement now the share_redex function:
void share_redexes(TMem* tm, APair* steal, u32 tid) {
  const u64 NEED_REDEX = 0xFFFFFFFFFFFFFFFF;

  // Gets the peer ID
  u32 pid = peer_id(tid, TPC_L2, tm->tick);
  u32 idx = buck_id(tid, TPC_L2, tm->tick);

  // Gets a redex from parent peer
  if (tid > pid && tm->rbag.lo_idx == 0) {
    Pair peek_redex = atomic_load(&steal[idx]);
    if (peek_redex == 0) {
      atomic_exchange(&steal[idx], NEED_REDEX);
    }
    if (peek_redex > 0 && peek_redex != NEED_REDEX) {
      push_redex(tm, peek_redex);
      atomic_store(&steal[idx], 0);
    }
  }

  // Sends a redex to child peer
  if (tid < pid && tm->rbag.lo_idx > 1) {
    Pair peek_redex = atomic_load(&steal[idx]);
    if (peek_redex == NEED_REDEX) {
      atomic_store(&steal[idx], pop_redex(tm));
    }
  }
}

// Interactions
// ------------

// The Link Interaction.
static inline bool interact_link(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.
  if (!get_resources(net, tm, 1, 0, 0)) {
    return false;
  }
  
  // Links.
  link_pair(net, tm, new_pair(a, b));

  return true;
}

// The Call Interaction.
static inline bool interact_call(GNet* net, TMem* tm, Port a, Port b, Book* book) {
  u32  fid = get_val(a);
  Def* def = &book->defs_buf[fid];
  
  // Allocates needed nodes and vars.
  if (!get_resources(net, tm, def->rbag_len + 1, def->node_len - 1, def->vars_len)) {
    return false;
  }

  // Stores new vars.  
  for (u32 i = 0; i < def->vars_len; ++i) {
    vars_create(net, tm->vars_loc[i], new_port(VAR, tm->vars_loc[i]));
    //printf("vars_create vars_loc[%04x] %04x\n", i, tm->vars_loc[i]);
  }

  // Stores new nodes.  
  for (u32 i = 1; i < def->node_len; ++i) {
    node_create(net, tm->node_loc[i-1], adjust_pair(net, tm, def->node_buf[i]));
    //printf("node_create node_loc[%04x] %08llx\n", i-1, def->node_buf[i]);
  }

  // Links.
  link_pair(net, tm, new_pair(b, adjust_port(net, tm, get_fst(def->node_buf[0]))));
  for (u32 i = 0; i < def->rbag_len; ++i) {
    link_pair(net, tm, adjust_pair(net, tm, def->rbag_buf[i]));
  }

  return true;
}

// The Void Interaction.  
static inline bool interact_void(GNet* net, TMem* tm, Port a, Port b) {
  return true;
}

// The Eras Interaction.
static inline bool interact_eras(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.  
  if (!get_resources(net, tm, 2, 0, 0)) {
    return false;
  }

  // Checks availability
  if (node_load(net, get_val(b)) == 0) {
    //printf("[%04x] unavailable0: %s\n", tid, show_port(b).x);
    return false;
  }

  // Loads ports.
  Pair B  = node_exchange(net, get_val(b), 0);
  Port B1 = get_fst(B);
  Port B2 = get_snd(B);
  
  //if (B == 0) printf("[%04x] ERROR2: %s\n", tid, show_port(b).x);

  // Links.
  link_pair(net, tm, new_pair(a, B1));
  link_pair(net, tm, new_pair(a, B2));

  return true;
}

// The Anni Interaction.  
static inline bool interact_anni(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.
  if (!get_resources(net, tm, 2, 0, 0)) {
    //printf("AAA\n");
    return false;
  }

  // Checks availability
  if (node_load(net, get_val(a)) == 0 || node_load(net, get_val(b)) == 0) {
    //printf("[%04x] unavailable1: %s | %s\n", tid, show_port(a).x, show_port(b).x);
    //printf("BBB\n");
    return false;
  }

  // Loads ports.
  Pair A  = node_take(net, get_val(a));
  Port A1 = get_fst(A);
  Port A2 = get_snd(A);
  Pair B  = node_take(net, get_val(b));
  Port B1 = get_fst(B);
  Port B2 = get_snd(B);
      
  //if (A == 0) printf("[%04x] ERROR3: %s\n", tid, show_port(a).x);
  //if (B == 0) printf("[%04x] ERROR4: %s\n", tid, show_port(b).x);

  // Links.  
  link_pair(net, tm, new_pair(A1, B1));
  link_pair(net, tm, new_pair(A2, B2));

  return true;
}

// The Comm Interaction.
static inline bool interact_comm(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.  
  if (!get_resources(net, tm, 4, 4, 4)) {
    return false;
  }

  // Checks availability
  if (node_load(net, get_val(a)) == 0 || node_load(net, get_val(b)) == 0) {
    //printf("[%04x] unavailable2: %s | %s\n", tid, show_port(a).x, show_port(b).x);
    return false;
  }

  // Loads ports.  
  Pair A  = node_take(net, get_val(a));
  Port A1 = get_fst(A);
  Port A2 = get_snd(A);
  Pair B  = node_take(net, get_val(b));
  Port B1 = get_fst(B);
  Port B2 = get_snd(B);
      
  //if (A == 0) printf("[%04x] ERROR5: %s\n", tid, show_port(a).x);
  //if (B == 0) printf("[%04x] ERROR6: %s\n", tid, show_port(b).x);

  // Stores new vars.
  vars_create(net, tm->vars_loc[0], new_port(VAR, tm->vars_loc[0]));
  vars_create(net, tm->vars_loc[1], new_port(VAR, tm->vars_loc[1]));
  vars_create(net, tm->vars_loc[2], new_port(VAR, tm->vars_loc[2]));
  vars_create(net, tm->vars_loc[3], new_port(VAR, tm->vars_loc[3]));
  
  // Stores new nodes.
  node_create(net, tm->node_loc[0], new_pair(new_port(VAR, tm->vars_loc[0]), new_port(VAR, tm->vars_loc[1])));
  node_create(net, tm->node_loc[1], new_pair(new_port(VAR, tm->vars_loc[2]), new_port(VAR, tm->vars_loc[3])));
  node_create(net, tm->node_loc[2], new_pair(new_port(VAR, tm->vars_loc[0]), new_port(VAR, tm->vars_loc[2])));
  node_create(net, tm->node_loc[3], new_pair(new_port(VAR, tm->vars_loc[1]), new_port(VAR, tm->vars_loc[3])));

  // Links.
  link_pair(net, tm, new_pair(A1, new_port(get_tag(b), tm->node_loc[0])));
  link_pair(net, tm, new_pair(A2, new_port(get_tag(b), tm->node_loc[1])));  
  link_pair(net, tm, new_pair(B1, new_port(get_tag(a), tm->node_loc[2])));
  link_pair(net, tm, new_pair(B2, new_port(get_tag(a), tm->node_loc[3])));

  return true;  
}

// The Oper Interaction.  
static inline bool interact_oper(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.
  if (!get_resources(net, tm, 1, 1, 0)) {
    return false;
  }

  // Checks availability  
  if (node_load(net, get_val(b)) == 0) {
    //printf("[%04x] unavailable3: %s\n", tid, show_port(b).x);
    return false;
  }

  // Loads ports.
  u32  av = get_val(a);
  Pair B  = node_take(net, get_val(b));
  Port B1 = get_fst(B);
  Port B2 = get_snd(B);
     
  //if (B == 0) printf("[%04x] ERROR8: %s\n", tid, show_port(b).x);
  
  // Performs operation.
  if (get_tag(B1) == NUM) {
    u32 bv = get_val(B1);
    u32 rv = av + bv;
    link_pair(net, tm, new_pair(B2, new_port(NUM, rv))); 
  } else {
    node_create(net, tm->node_loc[0], new_pair(a, B2));
    link_pair(net, tm, new_pair(B1, new_port(OPR, tm->node_loc[0])));
  }

  return true;  
}

// The Swit Interaction.
static inline bool interact_swit(GNet* net, TMem* tm, Port a, Port b) {
  // Allocates needed nodes and vars.  
  if (!get_resources(net, tm, 1, 2, 0)) {
    return false;
  }
  
  // Checks availability
  if (node_load(net,get_val(b)) == 0) {
    //printf("[%04x] unavailable4: %s\n", tid, show_port(b).x); 
    return false;
  }

  // Loads ports.
  u32  av = get_val(a);
  Pair B  = node_take(net, get_val(b));
  Port B1 = get_fst(B);
  Port B2 = get_snd(B);
 
  // Stores new nodes.  
  if (av == 0) {
    node_create(net, tm->node_loc[0], new_pair(B2, new_port(ERA,0)));
    link_pair(net, tm, new_pair(new_port(CON, tm->node_loc[0]), B1));
  } else {
    node_create(net, tm->node_loc[0], new_pair(new_port(ERA,0), new_port(CON, tm->node_loc[1])));
    node_create(net, tm->node_loc[1], new_pair(new_port(NUM, av-1), B2));
    link_pair(net, tm, new_pair(new_port(CON, tm->node_loc[0]), B1));
  }

  return true;
}

// Pops a local redex and performs a single interaction.
static inline bool interact(GNet* net, TMem* tm, Book* book) {
  // Pops a redex.
  Pair redex = pop_redex(tm);

  // If there is no redex, stop.
  if (redex != 0) {
    // Gets redex ports A and B.
    Port a = get_fst(redex);
    Port b = get_snd(redex);

    // Gets the rule type.
    Rule rule = get_rule(a, b);

    // Used for root redex.
    if (get_tag(a) == REF && get_tag(b) == VAR) {
      rule = CALL;
    // Swaps ports if necessary.  
    } else if (should_swap(a,b)) {
      swap(&a, &b);
    }

    //if (tid == 0) {
      //printf("REDUCE %s ~ %s | %s | rlen=%d\n", show_port(a).x, show_port(b).x, show_rule(rule).x, rbag_len(&tm->rbag));
    //}

    // Dispatches interaction rule.
    bool success;
    switch (rule) {
      case LINK: success = interact_link(net, tm, a, b); break;
      case CALL: success = interact_call(net, tm, a, b, book); break;
      case VOID: success = interact_void(net, tm, a, b); break;
      case ERAS: success = interact_eras(net, tm, a, b); break;
      case ANNI: success = interact_anni(net, tm, a, b); break;
      case COMM: success = interact_comm(net, tm, a, b); break; 
      case OPER: success = interact_oper(net, tm, a, b); break;
      case SWIT: success = interact_swit(net, tm, a, b); break;
    }

    // If error, pushes redex back.
    if (!success) {
      push_redex(tm, redex);
      return false;
    // Else, increments the interaction count.
    } else {
      tm->itrs += 1;
    }
  }

  return true;
}

// Evaluator
// ---------

void evaluator(GNet* net, TMem* tm, Book* book) {
  // Increments the tick
  tm->tick += 1;

  // Performs some interactions
  while (rbag_len(&tm->rbag) > 0) {
  //for (u32 i = 0; i < 16; ++i) {
    interact(net, tm, book);
  }

  // Shares a redex with neighbor thread
  //if (TPC > 1) {
    //share_redexes(tm, net->steal, tm->tid);
  //}

  atomic_fetch_add(&net->itrs, tm->itrs);
  tm->itrs = 0;
}

// Debug Printing
// --------------

void put_u32(char* B, u32 val) {
  for (int i = 0; i < 8; i++, val >>= 4) {
    B[8-i-1] = "0123456789ABCDEF"[val & 0xF];
  }  
}

Show show_port(Port port) {
  // NOTE: this is done like that because sprintf seems not to be working
  Show s;
  switch (get_tag(port)) {
    case VAR: memcpy(s.x, "VAR:", 4); put_u32(s.x+4, get_val(port)); break;
    case REF: memcpy(s.x, "REF:", 4); put_u32(s.x+4, get_val(port)); break;
    case ERA: memcpy(s.x, "ERA:________", 12); break;
    case NUM: memcpy(s.x, "NUM:", 4); put_u32(s.x+4, get_val(port)); break; 
    case CON: memcpy(s.x, "CON:", 4); put_u32(s.x+4, get_val(port)); break; 
    case DUP: memcpy(s.x, "DUP:", 4); put_u32(s.x+4, get_val(port)); break; 
    case OPR: memcpy(s.x, "OPR:", 4); put_u32(s.x+4, get_val(port)); break; 
    case SWI: memcpy(s.x, "SWI:", 4); put_u32(s.x+4, get_val(port)); break;
  }
  s.x[12] = '\0';
  return s;
}

Show show_rule(Rule rule) {
  Show s;
  switch (rule) {
    case LINK: memcpy(s.x, "LINK", 4); break;
    case VOID: memcpy(s.x, "VOID", 4); break;
    case ERAS: memcpy(s.x, "ERAS", 4); break;
    case ANNI: memcpy(s.x, "ANNI", 4); break;
    case COMM: memcpy(s.x, "COMM", 4); break;
    case OPER: memcpy(s.x, "OPER", 4); break;
    case SWIT: memcpy(s.x, "SWIT", 4); break;
    case CALL: memcpy(s.x, "CALL", 4); break;
    default  : memcpy(s.x, "????", 4); break;  
  }
  s.x[4] = '\0'; 
  return s;
}

void print_rbag(RBag* rbag) {
  printf("RBAG | FST-TREE     | SND-TREE    \n");
  printf("---- | ------------ | ------------\n");
  for (u32 i = 0; i < rbag->lo_idx; ++i) {
    Pair redex = rbag->buf[i];
    printf("%04X | %s | %s\n", i, show_port(get_fst(redex)).x, show_port(get_snd(redex)).x);
  }
  for (u32 i = 15; i > rbag->hi_idx; --i) {
    Pair redex = rbag->buf[i];
    printf("%04X | %s | %s\n", i, show_port(get_fst(redex)).x, show_port(get_snd(redex)).x);
  }  
  printf("==== | ============ | ============\n");
}

void print_net(GNet* net) {
  printf("NODE | PORT-1       | PORT-2      \n");
  printf("---- | ------------ | ------------\n");  
  for (u32 i = 0; i < G_NODE_LEN; ++i) {
    Pair node = node_load(net, i);
    if (node != 0) {
      printf("%04X | %s | %s\n", i, show_port(get_fst(node)).x, show_port(get_snd(node)).x);
    }
  }
  printf("==== | ============ |\n");  
  printf("VARS | VALUE        |\n");
  printf("---- | ------------ |\n");  
  for (u32 i = 0; i < G_VARS_LEN; ++i) {
    Port var = vars_load(net,i);
    if (var != 0) {
      printf("%04X | %s |\n", i, show_port(vars_load(net,i)).x);
    }
  }  
  printf("==== | ============ |\n");
}

// Example Books  
// -------------

Book BOOK = {
  6,
  {
    { // fun
      0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      4, { 0x000000000000000C, 0x000000000000001F, 0x0000001100000009, 0x0000000000000014, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },  
      1,
    },
    { // fun$C0
      1, { 0x0000000C00000019, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      2, { 0x0000000000000000, NUM|(LOOPS<<3), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      1,
    },
    { // fun$C1
      2, { 0x0000001C00000001, 0x0000002C00000001, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      6, { 0x000000000000000C, 0x0000001000000015, 0x0000000800000000, 0x0000002600000000, 0x0000001000000018, 0x0000001800000008, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      4,
    },
    { // loop
      0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      4, { 0x000000000000000C, 0x000000000000001F, 0x0000002100000003, 0x0000000000000014, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      1,  
    },
    { // loop$C0  
      1, { 0x0000001400000019, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      3, { 0x000000000000000C, 0x0000000800000000, 0x0000000800000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      2,
    },
    { // main  
      1, { 0x0000000C00000001, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      2, { 0x0000000000000000, NUM|(DEPTH<<3), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, },
      1,  
    },
  }
};

// Main  
// ----

int main() {
  // GMem
  GNet *gnet = malloc(sizeof(GNet));

  // TODO: copy the const book to a local allocated book here
  Book* book = malloc(sizeof(Book));
  memcpy(book, &BOOK, sizeof(Book));

  // TODO: alloc and init 16 TMem's
  TMem* tm[TPC];
  for (u32 t = 0; t < TPC; ++t) {
    tm[t] = malloc(sizeof(TMem));
    tmem_init(tm[t], t);
  }

  // Set the initial redex
  push_redex(tm[0], new_pair(new_port(REF, 5), new_port(VAR, 0)));

  // Evaluates
  evaluator(gnet, tm[0], book);

  // Interactions
  printf("itrs: %llu\n", atomic_load(&gnet->itrs));

  // Frees values
  for (u32 t = 0; t < TPC; ++t) {
    free(tm[t]);
  }
  free(gnet);
  free(book);

  return 0;
}

