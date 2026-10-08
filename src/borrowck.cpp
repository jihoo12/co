#include "borrowck.h"

#include <cstdint>
#include <set>
#include <unordered_map>

namespace co {
using namespace mir;

namespace {

class BitSet {
public:
  BitSet() = default;
  explicit BitSet(size_t n) : n_(n), w_((n + 63) / 64, 0) {}
  void set(size_t i) { w_[i / 64] |= 1ull << (i % 64); }
  void reset(size_t i) { w_[i / 64] &= ~(1ull << (i % 64)); }
  bool test(size_t i) const { return (w_[i / 64] >> (i % 64)) & 1; }
  bool unionWith(const BitSet &o) {
    bool changed = false;
    for (size_t i = 0; i < w_.size(); i++) {
      uint64_t nw = w_[i] | o.w_[i];
      changed |= nw != w_[i];
      w_[i] = nw;
    }
    return changed;
  }
  void setAll() {
    for (size_t i = 0; i < n_; i++)
      set(i);
  }
  bool operator==(const BitSet &o) const { return w_ == o.w_; }
  size_t size() const { return n_; }

private:
  size_t n_ = 0;
  std::vector<uint64_t> w_;
};

struct Access {
  enum Kind { Read, Move, SharedBorrow, MutBorrow, Write, ShallowWrite, Drop, StorageDead } kind;
  Place place;
  SourceLoc loc;
};

struct Loan {
  Place place;
  bool mut;
  int point;
  SourceLoc loc;
  int region;
};

class Checker {
public:
  Checker(const Function &f, Diagnostics &diag) : f_(f), diag_(diag) {}

  void run() {
    layout();
    checkInit();
    computeLiveness();
    collectLoansAndRegions();
    solveRegions();
    checkLoans();
  }

private:
  const Function &f_;
  Diagnostics &diag_;
  std::vector<int> base_; // first point of each block
  int numPoints_ = 0;
  std::vector<std::vector<int>> preds_;
  std::vector<BitSet> liveIn_; // per point
  std::vector<Loan> loans_;
  std::unordered_map<int, int> loanAt_; // point -> loan
  std::vector<int> localRegion_;         // local -> region (or -1)
  std::vector<BitSet> regions_;
  std::vector<std::pair<int, int>> outlives_; // (sup, sub): sup must contain sub

  int point(int bb, int i) const { return base_[bb] + i; }

  static std::vector<int> successors(const BasicBlock &bb) {
    switch (bb.term.kind) {
    case Terminator::Goto: return {bb.term.target};
    case Terminator::If: return {bb.term.thenBB, bb.term.elseBB};
    default: return {};
    }
  }

  void layout() {
    size_t nb = f_.blocks.size();
    base_.resize(nb);
    preds_.assign(nb, {});
    for (size_t b = 0; b < nb; b++) {
      base_[b] = numPoints_;
      numPoints_ += (int)f_.blocks[b].stmts.size() + 1;
      for (int s : successors(f_.blocks[b]))
        preds_[s].push_back((int)b);
    }
  }

  // ----- access enumeration -----

  static void indexReads(const Place &p, SourceLoc loc, std::vector<Access> &out) {
    for (auto &pr : p.proj)
      if (pr.kind == Proj::Index)
        out.push_back({Access::Read, Place{pr.indexLocal, {}}, loc});
  }

  static std::vector<Access> accessesOf(const Statement &s) {
    std::vector<Access> out;
    switch (s.kind) {
    case Statement::Assign:
      for (auto &op : s.rv.ops) {
        if (op.kind == Operand::Const)
          continue;
        indexReads(op.place, s.loc, out);
        out.push_back({op.kind == Operand::Move ? Access::Move : Access::Read, op.place, s.loc});
      }
      if (s.rv.kind == Rvalue::Ref) {
        indexReads(s.rv.place, s.loc, out);
        out.push_back({s.rv.mut ? Access::MutBorrow : Access::SharedBorrow, s.rv.place, s.loc});
      } else if (s.rv.kind == Rvalue::Discriminant) {
        indexReads(s.rv.place, s.loc, out);
        out.push_back({Access::Read, s.rv.place, s.loc});
      }
      indexReads(s.place, s.loc, out);
      out.push_back({s.place.isLocal() ? Access::ShallowWrite : Access::Write, s.place, s.loc});
      break;
    case Statement::Drop:
      out.push_back({Access::Drop, s.place, s.loc});
      break;
    case Statement::StorageDead:
      out.push_back({Access::StorageDead, Place{s.local, {}}, s.loc});
      break;
    case Statement::Nop:
      break;
    }
    return out;
  }

  static std::vector<Access> accessesOf(const Terminator &t, bool nonVoidReturn) {
    std::vector<Access> out;
    if (t.kind == Terminator::If && t.cond.kind != Operand::Const) {
      indexReads(t.cond.place, t.loc, out);
      out.push_back({Access::Read, t.cond.place, t.loc});
    }
    if (t.kind == Terminator::Return && nonVoidReturn)
      out.push_back({Access::Read, Place{0, {}}, t.loc});
    return out;
  }

  bool nonVoidReturn() const { return f_.info->ret->kind != TypeKind::Void; }

  // Moving out of a place consumes its whole local when the place is the
  // local itself or (for `opt or x`) the payload of an enum held in it.
  static bool consumesLocal(const Place &p) {
    for (auto &pr : p.proj)
      if (pr.kind != Proj::VariantField)
        return false;
    return true;
  }

  std::string name(const Place &p) const { return f_.placeName(p); }

  // ----- 1. initialization / move analysis -----

  void checkInit() {
    size_t nl = f_.locals.size(), nb = f_.blocks.size();
    std::vector<BitSet> entry(nb, BitSet(nl)); // maybe-uninitialized locals
    std::vector<bool> reached(nb, false);
    entry[0].setAll();
    for (int p = 1; p <= f_.numParams; p++)
      entry[0].reset(p);
    reached[0] = true;
    std::unordered_map<int, Note> movedAt;
    std::set<int> reported;

    auto transfer = [&](int b, BitSet st, bool report) {
      auto check = [&](const Access &a) {
        if (!report || !st.test(a.place.local) || reported.count(a.place.local))
          return;
        reported.insert(a.place.local);
        const Local &l = f_.locals[a.place.local];
        if (a.place.local == 0) {
          diag_.error(f_.endLoc, "missing return: function '" + f_.info->name +
                                     "' must return a value of type '" + f_.info->ret->str() + "'");
          return;
        }
        std::vector<Note> notes;
        auto mv = movedAt.find(a.place.local);
        if (mv != movedAt.end())
          notes.push_back(mv->second);
        std::string vname = l.name.empty() ? "temporary" : l.name;
        std::string msg;
        bool viaRef = !a.place.proj.empty() && a.place.proj[0].kind == Proj::Deref;
        if (a.kind == Access::Read || a.kind == Access::Move || viaRef)
          msg = "use of moved value '" + vname + "'";
        else if (a.kind == Access::Write)
          msg = "cannot assign to part of moved value '" + vname + "'";
        else
          msg = "borrow of moved value '" + vname + "'";
        if (!l.type->isCopy() && !l.type->isRef())
          msg += " (type '" + l.type->str() + "' is not copyable; use clone(...) or a reference)";
        diag_.error(a.loc, msg, notes);
      };
      const BasicBlock &bb = f_.blocks[b];
      for (auto &s : bb.stmts) {
        auto acc = accessesOf(s);
        for (auto &a : acc) {
          switch (a.kind) {
          case Access::Read:
          case Access::Move:
          case Access::SharedBorrow:
          case Access::MutBorrow:
          case Access::Write:
            check(a);
            break;
          default:
            break;
          }
        }
        for (auto &a : acc) {
          if (a.kind == Access::Move && consumesLocal(a.place)) {
            st.set(a.place.local);
            if (report)
              movedAt[a.place.local] = {a.loc, s.moveNote.empty() ? "value moved here" : s.moveNote};
          } else if (a.kind == Access::Drop || a.kind == Access::StorageDead) {
            st.set(a.place.local);
          }
        }
        for (auto &a : acc)
          if (a.kind == Access::ShallowWrite)
            st.reset(a.place.local);
      }
      for (auto &a : accessesOf(bb.term, nonVoidReturn()))
        check(a);
      return st;
    };

    // Fixpoint, then a reporting pass. Blocks are numbered roughly in
    // program order, which keeps "moved here" notes sensible.
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 0; b < nb; b++) {
        if (!reached[b])
          continue;
        BitSet out = transfer((int)b, entry[b], false);
        for (int s : successors(f_.blocks[b])) {
          if (!reached[s]) {
            reached[s] = true;
            entry[s] = out;
            changed = true;
          } else if (entry[s].unionWith(out)) {
            changed = true;
          }
        }
      }
    }
    for (size_t b = 0; b < nb; b++)
      if (reached[b])
        transfer((int)b, entry[b], true);
  }

  // ----- 2. liveness -----

  static void usesAndDefs(const std::vector<Access> &acc, std::vector<int> &uses, std::vector<int> &defs) {
    for (auto &a : acc) {
      switch (a.kind) {
      case Access::Read:
      case Access::Move:
      case Access::SharedBorrow:
      case Access::MutBorrow:
      case Access::Write:
        uses.push_back(a.place.local);
        break;
      case Access::ShallowWrite:
      case Access::StorageDead:
        defs.push_back(a.place.local);
        break;
      case Access::Drop:
        break;
      }
    }
  }

  void computeLiveness() {
    size_t nl = f_.locals.size(), nb = f_.blocks.size();
    std::vector<BitSet> liveOut(nb, BitSet(nl));
    liveIn_.assign(numPoints_, BitSet(nl));

    auto blockIn = [&](int b) {
      const BasicBlock &bb = f_.blocks[b];
      BitSet live = liveOut[b];
      auto step = [&](const std::vector<Access> &acc, int p) {
        std::vector<int> uses, defs;
        usesAndDefs(acc, uses, defs);
        for (int d : defs)
          live.reset(d);
        for (int u : uses)
          live.set(u);
        liveIn_[p] = live;
      };
      step(accessesOf(bb.term, nonVoidReturn()), point(b, (int)bb.stmts.size()));
      for (size_t i = bb.stmts.size(); i-- > 0;)
        step(accessesOf(bb.stmts[i]), point(b, (int)i));
      return live;
    };

    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = nb; b-- > 0;) {
        BitSet in = blockIn((int)b);
        for (int p : preds_[b])
          changed |= liveOut[p].unionWith(in);
      }
    }
    for (size_t b = 0; b < nb; b++)
      blockIn((int)b);
  }

  // ----- 3. regions -----

  int newRegion() {
    regions_.emplace_back(numPoints_);
    return (int)regions_.size() - 1;
  }

  void collectLoansAndRegions() {
    size_t nl = f_.locals.size();
    localRegion_.assign(nl, -1);
    for (size_t l = 0; l < nl; l++) {
      if (!f_.locals[l].type->containsRef())
        continue;
      int r = newRegion();
      localRegion_[l] = r;
      // A reference must be valid wherever it may still be used.
      for (int p = 0; p < numPoints_; p++)
        if (liveIn_[p].test(l))
          regions_[r].set(p);
    }

    for (size_t b = 0; b < f_.blocks.size(); b++) {
      const BasicBlock &bb = f_.blocks[b];
      for (size_t i = 0; i < bb.stmts.size(); i++) {
        const Statement &s = bb.stmts[i];
        if (s.kind != Statement::Assign)
          continue;
        int p = point((int)b, (int)i);
        int destRegion = s.place.isLocal() ? localRegion_[s.place.local] : -1;
        const Rvalue &rv = s.rv;
        if (rv.kind == Rvalue::Ref) {
          Loan loan{rv.place, rv.mut, p, s.loc, newRegion()};
          if (destRegion >= 0)
            outlives_.push_back({loan.region, destRegion});
          // A borrow must not outlive references held by what it borrows:
          // reborrowing through `*r` can't outlive `r`, and borrowing an
          // optional reference can't outlive the reference inside it.
          int rr = localRegion_[rv.place.local];
          if (rr >= 0)
            outlives_.push_back({rr, loan.region});
          loanAt_[p] = (int)loans_.size();
          loans_.push_back(loan);
        } else if (destRegion >= 0) {
          // Data flowing into a reference: the source must outlive the destination.
          // Builtins that return references (map lookups) borrow only from
          // their first operand, never from the key.
          size_t nops = rv.kind == Rvalue::Builtin ? std::min<size_t>(1, rv.ops.size()) : rv.ops.size();
          for (size_t oi = 0; oi < nops; oi++) {
            const Operand &op = rv.ops[oi];
            if (op.kind == Operand::Const || !op.type->containsRef())
              continue;
            int sr = localRegion_[op.place.local];
            if (sr >= 0)
              outlives_.push_back({sr, destRegion});
          }
        }
      }
    }
  }

  void solveRegions() {
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto [sup, sub] : outlives_)
        changed |= regions_[sup].unionWith(regions_[sub]);
    }
  }

  // ----- 4. loans in scope & conflicts -----

  // Do the loan's place and the accessed place overlap?
  static bool overlaps(const Place &a, const Place &b) {
    if (a.local != b.local)
      return false;
    size_t n = std::min(a.proj.size(), b.proj.size());
    for (size_t i = 0; i < n; i++) {
      const Proj &x = a.proj[i], &y = b.proj[i];
      if (x.kind == Proj::Field && y.kind == Proj::Field && x.field != y.field)
        return false; // disjoint fields
      if (x.kind == Proj::VariantField && y.kind == Proj::VariantField && x.variant == y.variant &&
          x.field != y.field)
        return false;
    }
    return true;
  }

  static bool throughDeref(const Place &p) {
    for (auto &pr : p.proj)
      if (pr.kind == Proj::Deref)
        return true;
    return false;
  }

  bool conflicts(const Loan &l, const Access &a) const {
    if (l.place.local != a.place.local)
      return false;
    switch (a.kind) {
    case Access::StorageDead:
      return !throughDeref(l.place);
    case Access::ShallowWrite:
      // Overwriting a reference doesn't affect what it pointed to.
      if (!l.place.proj.empty() && l.place.proj[0].kind == Proj::Deref)
        return false;
      return overlaps(l.place, a.place);
    case Access::Read:
    case Access::SharedBorrow:
      return l.mut && overlaps(l.place, a.place);
    default:
      return overlaps(l.place, a.place);
    }
  }

  void report(const Loan &l, const Access &a) {
    std::string p = name(a.place);
    std::string lp = name(l.place);
    std::string msg;
    std::string noteMsg = std::string(l.mut ? "mutable" : "immutable") + " borrow of '" + lp + "' occurs here";
    switch (a.kind) {
    case Access::Read:
      msg = "cannot use '" + p + "' because it is mutably borrowed";
      break;
    case Access::Move:
      msg = "cannot move out of '" + p + "' because it is borrowed";
      break;
    case Access::SharedBorrow:
      msg = "cannot borrow '" + p + "' as immutable because it is also borrowed as mutable";
      break;
    case Access::MutBorrow:
      msg = l.mut ? "cannot borrow '" + p + "' as mutable more than once at a time"
                  : "cannot borrow '" + p + "' as mutable because it is also borrowed as immutable";
      break;
    case Access::Write:
    case Access::ShallowWrite:
      msg = "cannot assign to '" + p + "' because it is borrowed";
      break;
    case Access::Drop:
    case Access::StorageDead:
      if (f_.locals[a.place.local].name.empty())
        msg = "temporary value dropped while still borrowed";
      else
        msg = "'" + p + "' does not live long enough (it is dropped here while still borrowed)";
      noteMsg = "'" + lp + "' is borrowed here, and the borrow is used later";
      break;
    }
    diag_.error(a.loc, msg, {{l.loc, noteMsg}});
  }

  void checkLoans() {
    size_t nb = f_.blocks.size(), nloans = loans_.size();
    if (nloans == 0)
      return;
    std::vector<BitSet> entry(nb, BitSet(nloans));

    auto run = [&](int b, BitSet st, bool doReport) {
      const BasicBlock &bb = f_.blocks[b];
      auto filter = [&](int p) {
        for (size_t l = 0; l < nloans; l++)
          if (st.test(l) && !regions_[loans_[l].region].test(p))
            st.reset(l);
      };
      auto checkAccesses = [&](const std::vector<Access> &acc, int p) {
        if (!doReport)
          return;
        for (auto &a : acc) {
          bool atNext = a.kind == Access::Write || a.kind == Access::ShallowWrite;
          for (size_t l = 0; l < nloans; l++) {
            if (!st.test(l))
              continue;
            // A write to the destination happens after the operands were
            // consumed, so only loans still live afterwards matter.
            if (atNext && !regions_[loans_[l].region].test(p + 1))
              continue;
            if (conflicts(loans_[l], a)) {
              report(loans_[l], a);
              break;
            }
          }
        }
      };
      for (size_t i = 0; i < bb.stmts.size(); i++) {
        int p = point(b, (int)i);
        filter(p);
        const Statement &s = bb.stmts[i];
        checkAccesses(accessesOf(s), p);
        // kills
        if (s.kind == Statement::Assign && s.place.isLocal()) {
          for (size_t l = 0; l < nloans; l++)
            if (loans_[l].place.local == s.place.local && throughDeref(loans_[l].place))
              st.reset(l);
        } else if (s.kind == Statement::StorageDead) {
          for (size_t l = 0; l < nloans; l++)
            if (loans_[l].place.local == s.local)
              st.reset(l);
        }
        auto g = loanAt_.find(p);
        if (g != loanAt_.end())
          st.set(g->second);
      }
      int tp = point(b, (int)bb.stmts.size());
      filter(tp);
      checkAccesses(accessesOf(bb.term, nonVoidReturn()), tp);
      return st;
    };

    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 0; b < nb; b++) {
        BitSet out = run((int)b, entry[b], false);
        for (int s : successors(f_.blocks[b]))
          changed |= entry[s].unionWith(out);
      }
    }
    for (size_t b = 0; b < nb; b++)
      run((int)b, entry[b], true);
  }
};

} // namespace

void borrowCheck(const mir::Function &f, Diagnostics &diag) {
  Checker c(f, diag);
  c.run();
}

} // namespace co
