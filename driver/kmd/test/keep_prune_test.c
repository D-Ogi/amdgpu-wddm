/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host test of driver/kmd/guard_keep_prune.h, the selection behind KeepLog's bounded directory.
 *
 * The property: after every name in C:\BC250\kmdlog has been offered, in any order, the files that stay are exactly
 * the BC250_KEEP_FILES newest ring files of ours, and nothing that is not a ring file of ours is ever named for
 * deletion. The order the file system enumerates in is not ours to choose, so the model offers the same set
 * ascending, descending and shuffled with a seeded generator, and checks the answer against the set it generated.
 *
 * What the host cannot drive is in guard.c: ZwQueryDirectoryFile, ZwDeleteFile and the two passes over the real
 * directory. What it can drive is the whole decision. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "guard_keep_prune.h"

static unsigned checks, bad;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

#define NAMES 500u

static BC250_KEEP_PRUNE table;                  /* 25 KB: never on the stack, in the driver or here */
static wchar_t names[NAMES][BC250_KEEP_NAME];   /* generated oldest first, so the index is the age */
static unsigned order[NAMES];

static unsigned length(const wchar_t* text)
{
    unsigned n = 0;
    while (text[n] != 0) ++n;
    return n;
}

/* ring-<date>-<time>-<ms>-<label>.log, ascending in time with the index. One second per step, so the hour, minute
 * and second fields all roll over inside the set and a comparison that is not purely ordinal shows up. */
static void generate(void)
{
    unsigned i;
    for (i = 0; i < NAMES; ++i) {
        unsigned second = i % 60u, minute = (i / 60u) % 60u, hour = (i / 3600u) % 24u, day = 7u + i / 86400u;
        swprintf(names[i], BC250_KEEP_NAME, L"ring-202610%02u-%02u%02u%02u-%03u-%ls.log",
                 day, hour, minute, second, i % 1000u, (i % 2u) ? L"start" : L"stop");
        order[i] = i;
    }
}

static void offer_in_order(void)
{
    unsigned i;
    Bc250KeepPruneBegin(&table);
    for (i = 0; i < NAMES; ++i) CHECK(Bc250KeepPruneOffer(&table, names[order[i]], length(names[order[i]])) == 1);
}

/* Every name older than the limit goes, every newer one stays, and the count agrees. */
static void check_selection(unsigned offered)
{
    unsigned i, going = (offered > BC250_KEEP_FILES) ? offered - BC250_KEEP_FILES : 0u;
    CHECK(table.Offered == offered);
    CHECK(Bc250KeepPruneGoing(&table) == going);
    CHECK((Bc250KeepPruneLimit(&table) != 0) == (going != 0));
    for (i = 0; i < offered; ++i) {
        int goes = Bc250KeepPruneGoes(&table, names[i], length(names[i]));
        CHECK(goes == (int)(i < going));
    }
    if (going != 0) {
        /* The limit is the oldest name that stays, and the table holds the newest BC250_KEEP_FILES ascending. */
        CHECK(Bc250KeepCompare(Bc250KeepPruneLimit(&table), names[going]) == 0);
        CHECK(table.Count == BC250_KEEP_FILES);
        for (i = 0; i < BC250_KEEP_FILES; ++i) CHECK(Bc250KeepCompare(table.Newest[i], names[going + i]) == 0);
    } else {
        CHECK(table.Count == offered);
    }
}

/* xorshift32: the same shuffle on every machine, so a failure is reproducible. */
static unsigned long seed;
static unsigned long next_random(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

int main(void)
{
    unsigned i, j, run, counted;
    wchar_t long_name[BC250_KEEP_NAME + 8];

    generate();

    /* Ascending, which is what NTFS gives: the insertion lands at the end every time. */
    offer_in_order();
    check_selection(NAMES);

    /* Descending: every name after the first BC250_KEEP_FILES is older than the oldest held, so the fast path
     * refuses them all, and the table must still hold the newest ones. */
    for (i = 0; i < NAMES; ++i) order[i] = NAMES - 1u - i;
    offer_in_order();
    check_selection(NAMES);

    /* Shuffled, five seeded runs. */
    for (run = 0; run < 5u; ++run) {
        seed = 0x1D872B41ul + run;
        for (i = 0; i < NAMES; ++i) order[i] = i;
        for (i = NAMES - 1u; i > 0; --i) {
            j = (unsigned)(next_random() % (i + 1u));
            { unsigned t = order[i]; order[i] = order[j]; order[j] = t; }
        }
        offer_in_order();
        check_selection(NAMES);
    }

    /* Under the limit, at the limit, one over it. */
    for (counted = 0; counted < 3u; ++counted) {
        unsigned offered = BC250_KEEP_FILES - 1u + counted;
        Bc250KeepPruneBegin(&table);
        for (i = 0; i < offered; ++i) CHECK(Bc250KeepPruneOffer(&table, names[i], length(names[i])) == 1);
        check_selection(offered);
    }

    /* An empty directory: nothing offered, nothing to delete. */
    Bc250KeepPruneBegin(&table);
    CHECK(table.Offered == 0 && table.Count == 0);
    CHECK(Bc250KeepPruneLimit(&table) == 0);
    CHECK(Bc250KeepPruneGoing(&table) == 0);

    /* What is not a ring file of ours is ignored, and never deleted, whatever the limit is. The pattern the driver
     * gives the file system already filters most of this; the predicate is the second check, because a pattern is
     * matched against the short name as well. */
    offer_in_order();
    for (i = 0; i < BC250_KEEP_NAME + 7u; ++i) long_name[i] = L'r';
    long_name[BC250_KEEP_NAME + 7u] = 0;
    {
        static const wchar_t* strangers[] = {
            L"", L".log", L"ring-", L"ring", L"ring.log", L"ring-x.txt", L"notring-20261007-000000-000.log",
            L"RING-20261007-000000-000-start.log", L"ring-20261007-000000-000-start.LOG"
        };
        unsigned before = table.Offered, ignored = table.Ignored;
        for (i = 0; i < sizeof(strangers) / sizeof(strangers[0]); ++i) {
            CHECK(Bc250KeepPruneOffer(&table, strangers[i], length(strangers[i])) == 0);
            CHECK(Bc250KeepPruneGoes(&table, strangers[i], length(strangers[i])) == 0);
        }
        CHECK(Bc250KeepPruneOffer(&table, long_name, length(long_name)) == 0);
        CHECK(Bc250KeepPruneGoes(&table, long_name, length(long_name)) == 0);
        CHECK(table.Offered == before);
        CHECK(table.Ignored == ignored + sizeof(strangers) / sizeof(strangers[0]) + 1u);
    }
    /* The selection is the one the ring files alone made. */
    check_selection(NAMES);

    /* A file of a driver before 0.7.216.10 carries no label and sorts by its time like any other. */
    {
        wchar_t unlabelled[BC250_KEEP_NAME];
        swprintf(unlabelled, BC250_KEEP_NAME, L"ring-20261006-235959-999.log");     /* a day before every name above */
        Bc250KeepPruneBegin(&table);
        for (i = 0; i < NAMES; ++i) CHECK(Bc250KeepPruneOffer(&table, names[i], length(names[i])) == 1);
        CHECK(Bc250KeepPruneOffer(&table, unlabelled, length(unlabelled)) == 1);
        CHECK(Bc250KeepPruneGoes(&table, unlabelled, length(unlabelled)) == 1);      /* the oldest of the lot: it goes */
        CHECK(Bc250KeepCompare(Bc250KeepPruneLimit(&table), names[NAMES - BC250_KEEP_FILES]) == 0);
    }

    printf("keep prune: %u checks, %u failures\n", checks, bad);
    return bad ? 1 : 0;
}
