#include <string.h>
#include "unity.h"
#include "slotsync_logic.h"

void setUp(void) {}
void tearDown(void) {}

#define S (1000000LL)
#define MIN (60 * S)
#define HOUR (60 * MIN)

/* A healthy box 11 minutes up: nothing holds a copy back. */
static ss_in_t box(ss_other_t other)
{
    return (ss_in_t){.other = other, .uptime_us = 11 * MIN, .fail_us = -1};
}

static ss_in_t valid(bool same, bool newer)
{
    ss_in_t in = box(SS_OTHER_VALID);
    in.same = same;
    in.newer = newer;
    return in;
}

#define ASSERT_OUT(state_, copy_, in_, req_)                    \
    do {                                                        \
        const ss_out_t o_ = ss_decide(&(in_), (req_));          \
        TEST_ASSERT_EQUAL_INT((state_), o_.state);              \
        TEST_ASSERT_EQUAL((copy_), o_.copy);                    \
        TEST_ASSERT_EQUAL((copy_), o_.why == SS_GO);            \
    } while (0)

/* 1: empty or erased other slot -> a sync after 10 minutes, not before */
static void test_empty_syncs_after_10_min_not_before(void)
{
    ss_in_t in = box(SS_OTHER_EMPTY);
    in.uptime_us = 0;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    in.uptime_us = SS_DELAY_US - 1;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    in.uptime_us = SS_DELAY_US;
    ASSERT_OUT(SLOTSYNC_EMPTY, true, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT64(10 * MIN, SS_DELAY_US);
}

/* 2: the same SHA-256 -> in sync, no copy (also before the 10 minutes and while held) */
static void test_same_sha_is_in_sync_without_copy(void)
{
    ss_in_t in = valid(true, false);
    ASSERT_OUT(SLOTSYNC_IN_SYNC, false, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT(SS_NOT_DUE, ss_decide(&in, SS_AUTO).why);
    in.uptime_us = 5 * S;
    in.pending = true;
    ASSERT_OUT(SLOTSYNC_IN_SYNC, false, in, SS_AUTO);
}

/* 3: a valid image built earlier -> differs -> sync */
static void test_older_valid_differs_and_syncs(void)
{
    ss_in_t in = valid(false, false);
    ASSERT_OUT(SLOTSYNC_DIFFERS, true, in, SS_AUTO);
    in.uptime_us = 9 * MIN;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
}

/* 4: a valid image built later -> other_newer, never copied by itself; Sync now refuses without force,
 * copies with it */
static void test_newer_valid_is_kept_unless_forced(void)
{
    ss_in_t in = valid(false, true);
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, false, in, SS_AUTO);
    in.uptime_us = 30 * HOUR;
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, false, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, false, in, SS_NOW);
    TEST_ASSERT_EQUAL_INT(SS_E_NEWER, ss_decide(&in, SS_NOW).why);
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, true, in, SS_FORCE);
    in.uptime_us = 1 * MIN; /* Sync now does not wait for the 10 minutes */
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, true, in, SS_FORCE);
}

/* 5: an invalid or aborted image is replaceable -> sync */
static void test_invalid_or_aborted_is_replaced(void)
{
    ss_in_t in = box(SS_OTHER_INVALID);
    ASSERT_OUT(SLOTSYNC_INVALID, true, in, SS_AUTO);
    in.newer = true; /* a failed update is newer, and still replaceable */
    ASSERT_OUT(SLOTSYNC_INVALID, true, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_INVALID, true, in, SS_NOW);
    in.uptime_us = 2 * MIN;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
}

/* 6: the running image pending verify -> waiting; Sync now refused */
static void test_pending_verify_waits(void)
{
    ss_in_t in = valid(false, false);
    in.pending = true;
    in.uptime_us = 2 * HOUR;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_NOW);
    TEST_ASSERT_EQUAL_INT(SS_E_PENDING, ss_decide(&in, SS_NOW).why);
    TEST_ASSERT_EQUAL_INT(SS_E_PENDING, ss_decide(&in, SS_FORCE).why);
    in.pending = false;
    ASSERT_OUT(SLOTSYNC_DIFFERS, true, in, SS_AUTO);
}

/* 7: an upload holds the flash -> waiting (Sync now refused), then a sync once it ends */
static void test_upload_holds_the_sync_until_it_ends(void)
{
    ss_in_t in = box(SS_OTHER_EMPTY);
    in.flash_busy = true;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT(SS_E_FLASH_BUSY, ss_decide(&in, SS_NOW).why);
    TEST_ASSERT_EQUAL_INT(SS_E_FLASH_BUSY, ss_decide(&in, SS_FORCE).why);
    /* the upload failed: released, the slot checked again, then the copy */
    in.flash_busy = false;
    in.other = SS_OTHER_UNKNOWN;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT(SS_E_NOT_READY, ss_decide(&in, SS_NOW).why);
    in.other = SS_OTHER_INVALID; /* a partial image */
    ASSERT_OUT(SLOTSYNC_INVALID, true, in, SS_AUTO);
}

/* 8: a failure -> waiting for 1 h, then a retry; Sync now may retry at once */
static void test_failure_backs_off_an_hour(void)
{
    ss_in_t in = valid(false, false);
    in.fail_us = 11 * MIN;
    in.uptime_us = 11 * MIN + 1;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    in.uptime_us = 11 * MIN + SS_RETRY_US - 1;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    in.uptime_us = 11 * MIN + SS_RETRY_US;
    ASSERT_OUT(SLOTSYNC_DIFFERS, true, in, SS_AUTO);
    in.uptime_us = 20 * MIN;
    ASSERT_OUT(SLOTSYNC_WAITING, true, in, SS_NOW);
    TEST_ASSERT_EQUAL_INT64(HOUR, SS_RETRY_US);
}

/* 9: once per boot after a success; Sync now still copies (also over the same image) */
static void test_once_per_boot_after_success(void)
{
    ss_in_t in = valid(true, false);
    in.done = true;
    ASSERT_OUT(SLOTSYNC_IN_SYNC, false, in, SS_AUTO);
    in.same = false; /* done blocks a second copy (slotsync.c clears it when a failed upload destroyed the copy) */
    ASSERT_OUT(SLOTSYNC_DIFFERS, false, in, SS_AUTO);
    in.other = SS_OTHER_EMPTY;
    ASSERT_OUT(SLOTSYNC_EMPTY, false, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_EMPTY, true, in, SS_NOW);
    in = valid(true, false);
    in.done = true;
    ASSERT_OUT(SLOTSYNC_IN_SYNC, true, in, SS_NOW);
}

/* a copy running: "syncing", no second one */
static void test_copy_running_is_syncing(void)
{
    ss_in_t in = box(SS_OTHER_EMPTY);
    in.copying = true;
    ASSERT_OUT(SLOTSYNC_SYNCING, false, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_SYNCING, false, in, SS_FORCE);
    TEST_ASSERT_EQUAL_INT(SS_E_SYNCING, ss_decide(&in, SS_FORCE).why);
}

/* 10: Boot other slot refused for an empty or invalid other slot (and while anything runs) */
static void test_boot_other_needs_a_valid_image(void)
{
    ss_in_t in = box(SS_OTHER_EMPTY);
    TEST_ASSERT_EQUAL_INT(SS_E_EMPTY, ss_boot_other(&in));
    in.other = SS_OTHER_INVALID;
    TEST_ASSERT_EQUAL_INT(SS_E_INVALID, ss_boot_other(&in));
    in.other = SS_OTHER_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(SS_E_NOT_READY, ss_boot_other(&in));
    in = valid(false, true);
    TEST_ASSERT_EQUAL_INT(SS_GO, ss_boot_other(&in));
    in = valid(false, false);
    in.uptime_us = 1 * S; /* no wait for the 10 minutes */
    TEST_ASSERT_EQUAL_INT(SS_GO, ss_boot_other(&in));
    in = valid(true, false);
    TEST_ASSERT_EQUAL_INT(SS_GO, ss_boot_other(&in));
    in.copying = true;
    TEST_ASSERT_EQUAL_INT(SS_E_SYNCING, ss_boot_other(&in));
    in.copying = false;
    in.flash_busy = true;
    TEST_ASSERT_EQUAL_INT(SS_E_FLASH_BUSY, ss_boot_other(&in));
    in.flash_busy = false;
    in.pending = true;
    TEST_ASSERT_EQUAL_INT(SS_E_PENDING, ss_boot_other(&in));
}

/* another project's firmware (or one without an app descriptor) counts as invalid: replaced by the
 * automatic sync after the 10 minutes, whatever its build stamp, and never booted */
static void test_foreign_image_is_replaced_and_never_booted(void)
{
    ss_in_t in = box(SS_OTHER_FOREIGN);
    in.newer = true; /* built later: no protection */
    ASSERT_OUT(SLOTSYNC_INVALID, true, in, SS_AUTO);
    in.uptime_us = SS_DELAY_US - 1;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
    ASSERT_OUT(SLOTSYNC_WAITING, true, in, SS_NOW); /* Sync now needs no force for it */
    TEST_ASSERT_EQUAL_INT(SS_E_FOREIGN, ss_boot_other(&in));
    in.uptime_us = 2 * HOUR;
    TEST_ASSERT_EQUAL_INT(SS_E_FOREIGN, ss_boot_other(&in));
    in.done = true; /* once per boot holds for it too */
    ASSERT_OUT(SLOTSYNC_INVALID, false, in, SS_AUTO);
}

/* the same build stamp with a different SHA-256 is "not older": other_newer, never overwritten without
 * force, bootable */
static void test_equal_build_stamp_counts_as_newer(void)
{
    ss_in_t in = valid(false, ss_counts_newer("Nov  1 2026", "14:02:11", "Nov  1 2026", "14:02:11"));
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, false, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT(SS_E_NEWER, ss_decide(&in, SS_NOW).why);
    ASSERT_OUT(SLOTSYNC_OTHER_NEWER, true, in, SS_FORCE);
    TEST_ASSERT_EQUAL_INT(SS_GO, ss_boot_other(&in));
    /* a stamp one second older is older */
    in = valid(false, ss_counts_newer("Nov  1 2026", "14:02:10", "Nov  1 2026", "14:02:11"));
    ASSERT_OUT(SLOTSYNC_DIFFERS, true, in, SS_AUTO);
}

/* the running image couldn't be read whole: no copy this run, said as such, never "waiting" for ever; the
 * other slot is still reported and bootable */
static void test_unreadable_running_image_copies_nothing(void)
{
    ss_in_t in = valid(false, false);
    in.no_source = true;
    in.uptime_us = 1 * MIN;
    ASSERT_OUT(SLOTSYNC_DIFFERS, false, in, SS_AUTO);
    in.uptime_us = 3 * HOUR;
    ASSERT_OUT(SLOTSYNC_DIFFERS, false, in, SS_AUTO);
    TEST_ASSERT_EQUAL_INT(SS_E_NO_SOURCE, ss_decide(&in, SS_NOW).why);
    TEST_ASSERT_EQUAL_INT(SS_E_NO_SOURCE, ss_decide(&in, SS_FORCE).why);
    TEST_ASSERT_EQUAL_INT(SS_GO, ss_boot_other(&in));
    in.other = SS_OTHER_EMPTY;
    ASSERT_OUT(SLOTSYNC_EMPTY, false, in, SS_AUTO);
    in.other = SS_OTHER_UNKNOWN;
    ASSERT_OUT(SLOTSYNC_WAITING, false, in, SS_AUTO);
}

static void test_build_stamp_reads_date_and_time(void)
{
    TEST_ASSERT_EQUAL_INT64(20261101140211LL, ss_build_stamp("Nov  1 2026", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(20260131000000LL, ss_build_stamp("Jan 31 2026", "00:00:00"));
    TEST_ASSERT_EQUAL_INT64(20251224235959LL, ss_build_stamp("Dec 24 2025", "23:59:59"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov  1 2026", ""));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Foo  1 2026", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov 1 2026", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov 32 2026", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov  1 2026", "24:00:00"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov  1 2026", "14:02"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp("Nov  1 2026x", "14:02:11"));
    TEST_ASSERT_EQUAL_INT64(-1, ss_build_stamp(NULL, "14:02:11"));
}

static void test_counts_newer_compares_date_then_time(void)
{
    TEST_ASSERT_TRUE(ss_counts_newer("Nov  2 2026", "00:00:01", "Nov  1 2026", "23:59:59"));
    TEST_ASSERT_TRUE(ss_counts_newer("Jan  1 2027", "00:00:00", "Dec 31 2026", "23:59:59"));
    TEST_ASSERT_TRUE(ss_counts_newer("Nov  1 2026", "14:02:12", "Nov  1 2026", "14:02:11"));
    /* the same second (a stale stamp) is not older */
    TEST_ASSERT_TRUE(ss_counts_newer("Nov  1 2026", "14:02:11", "Nov  1 2026", "14:02:11"));
    TEST_ASSERT_FALSE(ss_counts_newer("Nov  1 2026", "14:02:10", "Nov  1 2026", "14:02:11"));
    TEST_ASSERT_FALSE(ss_counts_newer("Oct 31 2026", "23:59:59", "Nov  1 2026", "00:00:00"));
    /* an image that can't be dated is kept: counts as newer */
    TEST_ASSERT_TRUE(ss_counts_newer("", "", "Nov  1 2026", "14:02:11"));
    TEST_ASSERT_TRUE(ss_counts_newer("Oct 31 2026", "23:59:59", "", ""));
}

static void test_state_names(void)
{
    TEST_ASSERT_EQUAL_STRING("in_sync", slotsync_state_name(SLOTSYNC_IN_SYNC));
    TEST_ASSERT_EQUAL_STRING("syncing", slotsync_state_name(SLOTSYNC_SYNCING));
    TEST_ASSERT_EQUAL_STRING("differs", slotsync_state_name(SLOTSYNC_DIFFERS));
    TEST_ASSERT_EQUAL_STRING("other_newer", slotsync_state_name(SLOTSYNC_OTHER_NEWER));
    TEST_ASSERT_EQUAL_STRING("empty", slotsync_state_name(SLOTSYNC_EMPTY));
    TEST_ASSERT_EQUAL_STRING("invalid", slotsync_state_name(SLOTSYNC_INVALID));
    TEST_ASSERT_EQUAL_STRING("waiting", slotsync_state_name(SLOTSYNC_WAITING));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_syncs_after_10_min_not_before);
    RUN_TEST(test_same_sha_is_in_sync_without_copy);
    RUN_TEST(test_older_valid_differs_and_syncs);
    RUN_TEST(test_newer_valid_is_kept_unless_forced);
    RUN_TEST(test_invalid_or_aborted_is_replaced);
    RUN_TEST(test_pending_verify_waits);
    RUN_TEST(test_upload_holds_the_sync_until_it_ends);
    RUN_TEST(test_failure_backs_off_an_hour);
    RUN_TEST(test_once_per_boot_after_success);
    RUN_TEST(test_copy_running_is_syncing);
    RUN_TEST(test_boot_other_needs_a_valid_image);
    RUN_TEST(test_foreign_image_is_replaced_and_never_booted);
    RUN_TEST(test_equal_build_stamp_counts_as_newer);
    RUN_TEST(test_unreadable_running_image_copies_nothing);
    RUN_TEST(test_build_stamp_reads_date_and_time);
    RUN_TEST(test_counts_newer_compares_date_then_time);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
