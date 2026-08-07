#pragma once

#include <stdexcept>
#include <string>

// Week 33. The identity of a bound column reference: a range-table POSITION
// plus WHICH range table it is a position in.
//
// Before this type, `relation_slot` was a bare int and `query_level` a separate
// field on the same struct, so any consumer could read one without the other.
// That collapse was found FIVE times in Week 30 alone (validateJoinCondition,
// classifyJoinCondition, GroupByColumn's round trip, exprKey, the SUM/AVG
// argument check) and twice more only when development.md's consumer table was
// itself audited for completeness. Every one of those failures was SILENT: an
// indexOf(name, slot) against the wrong block's schema is a clean HIT on the
// wrong relation, not a miss.
//
// !! THE SLOT IS PRIVATE, DELIBERATELY. There is no implicit conversion to int,
// no operator int(), and no public `slot` member. The whole value of the type is
// that a bare integer cannot be passed where a qualified reference is required —
// if it could, the migration would buy a one-time compiler-driven enumeration
// and nothing after it. Reading the slot costs a NAMED call that states which
// scope the reader believes it is in, and the name is what a future audit greps
// for.
//
// TWO DIFFERENT THINGS ARE CALLED A RELATION SLOT, and only one of them is this.
// ColumnDef::relation_slot (common/schema.h) is a SCHEMA slot: a Schema is built
// for exactly one query block, so there is no level to lose and every reader of
// it is safe by construction. It stays a bare int. This type is for slots that
// came from BINDER RESOLUTION — ColumnRef, GroupByColumn, AggregateSpec — which
// can name an ENCLOSING block's relation.
class ColumnId {
    public:
        ColumnId() = default;

        // Named constructors. `local(s)` is the overwhelmingly common case and
        // the one every pre-Week-30 call site meant; spelling it out is what
        // makes the rare `outer` sites visible in a grep.
        static ColumnId local(int slot)            { return ColumnId(0, slot); }
        static ColumnId outer(int level, int slot) { return ColumnId(level, slot); }
        static ColumnId unresolved()               { return ColumnId(); }

        // How many query blocks OUT the relation lives, relative to the block
        // the reference is WRITTEN in. 0 = this block's own range table.
        // RELATIVE, like Postgres's varlevelsup, so a subquery's tree means the
        // same thing wherever it sits.
        int level() const { return level_; }

        bool isLocal() const { return level_ == 0; }

        // slot >= 0. An UNRESOLVED id (the pre-binder state, and the value
        // hand-built test trees rely on) falls back to bare-name resolution at
        // every consumer, so this is a routing question and not an error.
        bool isResolved() const { return slot_ >= 0; }

        // THE NARROWING POINT for a reader whose own scope is the ref's scope.
        // Call this — there is no other way to obtain the integer for a local
        // read — anywhere the value is about to be used as a position in THIS
        // block's range table, or in a Schema built for this block. The name is
        // the claim; the throw is what stops the claim being wrong silently.
        // `site` names the caller so a planner defect points at itself rather
        // than at this header.
        int localSlot(const char* site) const {
            if (level_ != 0)
                throw std::runtime_error(
                    std::string("internal: ") + site + " read a correlated column "
                    "reference as a local relation slot (query level "
                    + std::to_string(level_) + ")");
            return slot_;
        }

        // THE ESCAPE HATCH, for the two readers that legitimately want the slot
        // WITHOUT being in its scope:
        //   - the Binder, which walks the scope chain out `level()` steps and
        //     then indexes THAT scope's range table — the only layer that can;
        //   - identity/keying (exprKey), where the pair is hashed as a whole and
        //     never used to index anything.
        // Anything else wanting this is almost certainly the collapse this type
        // exists to prevent. `site` is required so every use is greppable.
        int slotInOwnScope(const char* site) const {
            (void)site;
            return slot_;
        }

        // Decorrelation moves a reference from an inner block to the block that
        // supplies it, which is exactly a level decrement. It lives here so the
        // arithmetic has one home rather than being open-coded at each rewrite
        // site. Unused until Week 33's decorrelation lands; defined here because
        // the representation, not the feature, is what owns the operation.
        ColumnId outward() const {
            if (level_ == 0)
                throw std::runtime_error(
                    "internal: cannot move a local column reference outward");
            return ColumnId(level_ - 1, slot_);
        }

        bool operator==(const ColumnId& o) const {
            return level_ == o.level_ && slot_ == o.slot_;
        }
        bool operator!=(const ColumnId& o) const { return !(*this == o); }

        // "Could these two name the same relation?" — level must match exactly,
        // and an UNRESOLVED id on either side is a wildcard, because it will be
        // resolved by bare name at the consumer. checkGroupedRefs (validator.cc)
        // is the caller; the wildcard half is what lets a hand-built or
        // single-relation tree match a stamped group key.
        bool couldBeSameRelation(const ColumnId& o) const {
            if (level_ != o.level_) return false;
            if (!isResolved() || !o.isResolved()) return true;
            return slot_ == o.slot_;
        }

    private:
        ColumnId(int level, int slot) : level_(level), slot_(slot) {}
        int level_ = 0;
        int slot_ = -1;
};
