#include <gtest/gtest.h>
#include "common/value.h"
#include "common/schema.h"
#include "common/date_util.h"
#include "common/column_id.h"

// ===== value tests =====

TEST(ValueTest, IntConstruction) {
    Value v(int64_t(42));
    EXPECT_EQ(v.type(), TypeId::INT);
    EXPECT_EQ(v.asInt(), 42);
}

TEST(ValueTest, DoubleConstruction) {
    Value v(3.14);
    EXPECT_EQ(v.type(), TypeId::DOUBLE);
    EXPECT_DOUBLE_EQ(v.asDouble(), 3.14);
}

TEST(ValueTest, StringConstruction) {
    Value v(std::string("Ferrari"));
    EXPECT_EQ(v.type(), TypeId::STRING);
    EXPECT_EQ(v.asString(), "Ferrari");
}

TEST(ValueTest, IntComparison) {
    Value a(int64_t(5));
    Value b(int64_t(10));
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(a > b);
    EXPECT_TRUE(a != b);
}

TEST(ValueTest, StringComparison) {
    Value a(std::string("Ferrari"));
    Value b(std::string("McLaren"));
    EXPECT_TRUE(a < b);   // lexicographic
    EXPECT_TRUE(a != b);
}

TEST(ValueTest, TypeMismatchThrows) {
    Value a(int64_t(5));
    Value b(std::string("Ferrari"));
    EXPECT_THROW(a < b, std::runtime_error);
}

TEST(ValueTest, NullDefaultConstruction) {
    Value v;
    EXPECT_TRUE(v.isNull());
}

TEST(ValueTest, NullFactory) {
    Value v = Value::null();
    EXPECT_TRUE(v.isNull());
}

TEST(ValueTest, NonNullIsNotNull) {
    Value v(int64_t(42));
    EXPECT_FALSE(v.isNull());
}

TEST(ValueTest, NullComparisonReturnsFalse) {
    Value null_v;
    Value int_v(int64_t(42));
    EXPECT_FALSE(null_v == int_v);
    EXPECT_FALSE(int_v == null_v);
    EXPECT_FALSE(null_v == null_v);  // NULL = NULL is false in SQL
}

TEST(ValueTest, NullToString) {
    Value v;
    EXPECT_EQ(v.toString(), "NULL");
}

// ===== schema tests =====

TEST(SchemaTest, IndexOf) {
    Schema s({{"lap_id", TypeId::INT},
              {"team",   TypeId::STRING},
              {"speed",  TypeId::DOUBLE}});
    EXPECT_EQ(s.indexOf("team"), 1);
    EXPECT_EQ(s.indexOf("speed"), 2);
    EXPECT_EQ(s.indexOf("missing"), -1);
}

TEST(SchemaTest, HasColumn) {
    Schema s({{"season", TypeId::INT}});
    EXPECT_TRUE(s.hasColumn("season"));
    EXPECT_FALSE(s.hasColumn("bogus"));
}

// ===== row tests =====

TEST(RowTest, ConstructAndAccess) {
    Row row = {Value(int64_t(1)), Value(std::string("Ferrari")), Value(312.4)};
    EXPECT_EQ(row.size(), 3);
    EXPECT_EQ(row[1].asString(), "Ferrari");
}
// ===== date_util tests (Week 25) =====
//
// Dates are ISO-8601 STRING Values; these helpers back the parser's literal
// validation and the constant folder's interval arithmetic. C++17 has no
// std::chrono::year_month_day, so the civil-date conversion is hand-rolled and
// has to be pinned down here.

TEST(DateUtil, IsIsoDateAcceptsValidAndRejectsInvalid) {
    EXPECT_TRUE(isIsoDate("1998-12-01"));
    EXPECT_TRUE(isIsoDate("2024-02-29"));   // leap year
    EXPECT_FALSE(isIsoDate("2023-02-29"));  // not a leap year
    EXPECT_FALSE(isIsoDate("1998-02-30"));  // real calendar check, not just digits
    EXPECT_FALSE(isIsoDate("1998-13-01"));  // month out of range
    EXPECT_FALSE(isIsoDate("1998-00-01"));
    EXPECT_FALSE(isIsoDate("1998-12-00"));
    EXPECT_FALSE(isIsoDate("12/01/1998")); // wrong separator
    EXPECT_FALSE(isIsoDate("1998-12-1"));  // wrong width
    EXPECT_FALSE(isIsoDate("199a-12-01")); // non-digit
}

TEST(DateUtil, CivilDayRoundTrip) {
    // the epoch and both directions across it
    EXPECT_EQ(daysFromCivil(1970, 1, 1), 0);
    int y = 0, m = 0, d = 0;
    civilFromDays(0, y, m, d);
    EXPECT_EQ(y, 1970); EXPECT_EQ(m, 1); EXPECT_EQ(d, 1);

    for (int64_t z : {-100000, -1, 1, 12345, 20000}) {
        civilFromDays(z, y, m, d);
        EXPECT_EQ(daysFromCivil(y, m, d), z) << "round trip failed at day " << z;
    }
}

TEST(DateUtil, AddDaysCrossesMonthAndYearBoundaries) {
    // TPC-H Q1: date '1998-12-01' - interval '90' day
    EXPECT_EQ(addDays("1998-12-01", -90), "1998-09-02");
    EXPECT_EQ(addDays("1998-12-31", 1), "1999-01-01");
    EXPECT_EQ(addDays("2024-02-28", 1), "2024-02-29");   // leap day exists
    EXPECT_EQ(addDays("2023-02-28", 1), "2023-03-01");   // and does not
    EXPECT_EQ(addDays("1998-12-01", 0), "1998-12-01");
}

TEST(DateUtil, AddMonthsClampsTheDayToTheTargetMonth) {
    // TPC-H uses + interval '1' year / '3' month / '1' month
    EXPECT_EQ(addMonths("1994-01-01", 12), "1995-01-01");
    EXPECT_EQ(addMonths("1994-01-01", 3), "1994-04-01");
    // clamping: January 31 has no counterpart in February
    EXPECT_EQ(addMonths("2024-01-31", 1), "2024-02-29");  // leap
    EXPECT_EQ(addMonths("2023-01-31", 1), "2023-02-28");  // non-leap
    EXPECT_EQ(addMonths("2024-02-29", 12), "2025-02-28"); // leap day + 1 year
    EXPECT_EQ(addMonths("2024-03-31", -1), "2024-02-29"); // backwards clamps too
    EXPECT_EQ(addMonths("2024-01-15", -1), "2023-12-15"); // year rolls back
}

// Date arithmetic must never render an unrepresentable year. formatIsoDate
// writes exactly four digits, so year 101994 wrapped modulo 10000 back to
// "1994" — `date '1994-01-01' + interval '100000' year` silently returned the
// INPUT date with no error — and a negative year emitted a non-digit byte
// ('0' + -6 is '*'), producing "000*-01-01", a non-date string that then flowed
// into comparisons and zone maps as an ordinary STRING.
TEST(DateUtil, RejectsResultsOutsideTheRepresentableRange) {
    EXPECT_THROW(addYears("1994-01-01", 100000), std::runtime_error);   // wrapped to 1994
    EXPECT_THROW(addYears("1994-01-01", -2000), std::runtime_error);    // rendered '*'
    EXPECT_THROW(addMonths("1994-01-01", 12 * 100000), std::runtime_error);
    EXPECT_THROW(addDays("1994-01-01", 4000000000LL), std::runtime_error);

    // the boundaries themselves still work
    EXPECT_EQ(addYears("1994-01-01", 8005), "9999-01-01");
    EXPECT_EQ(addYears("1994-01-01", -1994), "0000-01-01");
    EXPECT_THROW(addYears("1994-01-01", 8006), std::runtime_error);
    EXPECT_THROW(addYears("1994-01-01", -1995), std::runtime_error);
}

// The counts are bounded BEFORE any arithmetic, so no addition can overflow.
// UBSan previously reported signed-integer overflow at all three sites
// (date_util.h addMonths/addDays and the negation in constant_folding.cc) —
// the engine ships checked_arith.h for exactly this, and date folding had been
// bypassing it.
TEST(DateUtil, ExtremeIntervalCountsDoNotOverflow) {
    const int64_t kMax = 9223372036854775807LL;
    EXPECT_THROW(addDays("1994-01-01", kMax), std::runtime_error);
    EXPECT_THROW(addDays("1994-01-01", -kMax), std::runtime_error);
    EXPECT_THROW(addMonths("1994-01-01", kMax), std::runtime_error);
    EXPECT_THROW(addMonths("1994-01-01", -kMax), std::runtime_error);
    EXPECT_THROW(addYears("1994-01-01", kMax), std::runtime_error);
    EXPECT_THROW(addYears("1994-01-01", -kMax), std::runtime_error);
}

// ===== ColumnId (Week 33) =====

// The narrowing point is the whole reason the slot is private. Without this
// test localSlot() is a comment with extra steps: nothing in the shipped
// queries reaches it with a correlated id yet, so a future edit that dropped
// the throw would pass every other test in the suite.
TEST(ColumnIdTest, LocalSlotRefusesACorrelatedReference) {
    EXPECT_EQ(ColumnId::local(2).localSlot("test"), 2);
    EXPECT_EQ(ColumnId::unresolved().localSlot("test"), -1);

    ColumnId correlated = ColumnId::outer(1, 0);
    EXPECT_THROW(correlated.localSlot("test"), std::runtime_error);
    // ...and the message names the caller, so a planner defect points at itself
    try {
        correlated.localSlot("SomeConsumer");
        ADD_FAILURE() << "a correlated id must not narrow to a local slot";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("SomeConsumer"), std::string::npos)
            << e.what();
    }
}

// unresolved() must be (level 0, slot -1), not (-1, -1): -1 is the "resolve by
// bare name" sentinel that every hand-built test tree and every single-relation
// consumer relies on, and a non-zero level on it would make isLocal() false for
// every unbound ref in the engine.
TEST(ColumnIdTest, UnresolvedIsLocalAndUnresolved) {
    ColumnId u;
    EXPECT_TRUE(u.isLocal());
    EXPECT_FALSE(u.isResolved());
    EXPECT_EQ(u, ColumnId::unresolved());
}

// Decorrelation (Week 33's feature half) moves a reference to the block that
// supplies it, which is exactly a level decrement. The arithmetic lives on the
// type so it has one home rather than being open-coded at each rewrite site.
TEST(ColumnIdTest, OutwardDecrementsTheLevelAndKeepsTheSlot) {
    EXPECT_EQ(ColumnId::outer(1, 3).outward(), ColumnId::local(3));
    EXPECT_EQ(ColumnId::outer(2, 3).outward(), ColumnId::outer(1, 3));
    EXPECT_THROW(ColumnId::local(0).outward(), std::runtime_error);
}

// couldBeSameRelation is checkGroupedRefs' matching rule, and both halves are
// load-bearing: the LEVEL must match exactly (a correlated group key must not
// satisfy an ungrouped local column of the same name -- Week 30 round 2), while
// an UNRESOLVED id on either side stays a wildcard, which is what lets a
// hand-built or single-relation tree match a stamped key.
TEST(ColumnIdTest, CouldBeSameRelationComparesLevelsExactlyAndSlotsLoosely) {
    EXPECT_TRUE(ColumnId::local(1).couldBeSameRelation(ColumnId::local(1)));
    EXPECT_FALSE(ColumnId::local(1).couldBeSameRelation(ColumnId::local(0)));
    EXPECT_FALSE(ColumnId::local(0).couldBeSameRelation(ColumnId::outer(1, 0)));
    EXPECT_TRUE(ColumnId::unresolved().couldBeSameRelation(ColumnId::local(3)));
    EXPECT_FALSE(ColumnId::unresolved().couldBeSameRelation(ColumnId::outer(1, 3)));
}
