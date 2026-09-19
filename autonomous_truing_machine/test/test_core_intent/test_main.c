/* Operator intents, wait-instance correlation and per-intent admissibility (SPEC §7.3, §12.3, §12.5). */
#include <string.h>
#include <unity.h>

#include "truing/debug_code.h"
#include "truing/operator_intent.h"

static truing_wait_correlator_t g_c;

void setUp(void)
{
    truing_wait_correlator_init(&g_c);
}
void tearDown(void) {}

static truing_wait_prompt_t apply_prompt(uint8_t spoke, float turns)
{
    truing_wait_prompt_t p;
    memset(&p, 0, sizeof(p));
    p.kind = TRUING_WAIT_APPLY_ADJUSTMENT;
    p.station = TRUING_STATION_ADJUSTMENT;
    p.target_index = spoke;
    p.display_turns_rev = turns;
    return p;
}

static truing_intent_t confirm_adjust(uint32_t wait_id)
{
    truing_intent_t i;
    memset(&i, 0, sizeof(i));
    i.type = TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
    i.wait_id = wait_id;
    return i;
}

static void test_wait_ids_are_monotonic_and_prompts_are_typed(void)
{
    truing_wait_prompt_t p = apply_prompt(5u, 0.25f);
    TEST_ASSERT_EQUAL_UINT32(1u, truing_wait_issue(&g_c, &p));
    TEST_ASSERT_TRUE(g_c.active);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE, g_c.prompt.expected_intent);
    TEST_ASSERT_EQUAL_UINT8(5u, g_c.prompt.target_index);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.25f, g_c.prompt.display_turns_rev);
    TEST_ASSERT_EQUAL_UINT32(0u, g_c.prompt.timeout_ms);   /* default: wait indefinitely */
    truing_wait_clear(&g_c);
    TEST_ASSERT_FALSE(g_c.active);
    TEST_ASSERT_EQUAL_UINT32(2u, truing_wait_issue(&g_c, &p));
    TEST_ASSERT_EQUAL_UINT32(3u, truing_wait_issue(&g_c, &p));
    /* Unknown kind cannot be issued. */
    p.kind = TRUING_WAIT_KIND_UNSET;
    TEST_ASSERT_EQUAL_UINT32(0u, truing_wait_issue(&g_c, &p));
    TEST_ASSERT_EQUAL_UINT32(0u, truing_wait_issue(&g_c, NULL));
}

static void test_positioning_prompt_must_name_a_station(void)
{
    /* SPEC §10A.3: a positioning request always names the feature AND the station. */
    truing_wait_prompt_t p;
    memset(&p, 0, sizeof(p));
    p.kind = TRUING_WAIT_POSITION_TO_SPOKE;
    p.target_index = 12u;
    TEST_ASSERT_EQUAL_UINT32(0u, truing_wait_issue(&g_c, &p));
    p.station = TRUING_STATION_ACOUSTIC_LEFT;
    TEST_ASSERT_EQUAL_UINT32(1u, truing_wait_issue(&g_c, &p));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_CONFIRM_POSITIONED, g_c.prompt.expected_intent);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC_LEFT, g_c.prompt.station);
    TEST_ASSERT_TRUE(truing_wait_kind_is_positioning(TRUING_WAIT_POSITION_TO_RIM_ANGLE));
    TEST_ASSERT_TRUE(truing_wait_kind_is_positioning(TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION));
    TEST_ASSERT_FALSE(truing_wait_kind_is_positioning(TRUING_WAIT_ENTER_RUNOUT));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_SUBMIT_RUNOUT, truing_wait_expected_intent(TRUING_WAIT_ENTER_RUNOUT));
}

static void test_wait_match_rules(void)
{
    truing_intent_t i = confirm_adjust(1u);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_MATCH_NO_ACTIVE_WAIT, truing_wait_match(&g_c, &i));
    truing_wait_prompt_t p = apply_prompt(5u, 0.25f);
    const uint32_t id = truing_wait_issue(&g_c, &p);
    i = confirm_adjust(id);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_MATCH_OK, truing_wait_match(&g_c, &i));
    i = confirm_adjust(id + 1u);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_MATCH_STALE_INTENT, truing_wait_match(&g_c, &i));
    i = confirm_adjust(0u);   /* client omitted wait_id: rejected, not accommodated (SPEC §12.3) */
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_MATCH_STALE_INTENT, truing_wait_match(&g_c, &i));
    i = confirm_adjust(id);
    i.type = TRUING_INTENT_CONFIRM_POSITIONED;
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_MATCH_WRONG_INTENT_TYPE, truing_wait_match(&g_c, &i));
}

static void test_spec_7_3_duplicate_confirmation_scenario(void)
{
    /* Spoke 5: wait issued, operator confirms; the message is delayed and sent twice. */
    truing_wait_prompt_t p5 = apply_prompt(5u, 0.25f);
    const uint32_t wait5 = truing_wait_issue(&g_c, &p5);
    truing_intent_t first = confirm_adjust(wait5);
    truing_intent_t duplicate = confirm_adjust(wait5);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT,
                          truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &first, false));
    truing_wait_clear(&g_c);
    /* System advances to spoke 6 and waits. */
    truing_wait_prompt_t p6 = apply_prompt(6u, -0.125f);
    const uint32_t wait6 = truing_wait_issue(&g_c, &p6);
    TEST_ASSERT_NOT_EQUAL(wait5, wait6);
    /* The delayed duplicate arrives: it MUST NOT confirm spoke 6. */
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STALE_INTENT,
                          truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &duplicate, false));
    TEST_ASSERT_EQUAL_UINT32(1u, g_c.stale_intents_discarded);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_STALE_INTENT, truing_intent_verdict_reason(TRUING_INTENT_REJECT_STALE_INTENT));
    /* The genuine spoke-6 confirmation is accepted. */
    truing_intent_t six = confirm_adjust(wait6);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT,
                          truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &six, false));
}

static void test_per_intent_admissibility(void)
{
    truing_intent_t i;
    memset(&i, 0, sizeof(i));
    i.type = TRUING_INTENT_START_TRUING;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_BOOT, &g_c, &i, false));

    /* ABORT: every operating state, including mid-measurement; not when nothing is running. */
    i.type = TRUING_INTENT_ABORT;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_MEASURE_SPOKE_TENSION, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_COMPUTE_ADJUSTMENTS, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_TERMINAL, &g_c, &i, false));

    /* SET_PARAMETER defers to the state x class rule. */
    i.type = TRUING_INTENT_SET_PARAMETER;
    i.payload.set_parameter.id = TRUING_PARAM_TARGET_TENSION;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_PARAMETER, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, false));
    i.payload.set_parameter.id = TRUING_PARAM_TOL_TENSION;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_PARAMETER, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, false));

    /* Confirmations need a matching active wait. */
    i = confirm_adjust(7u);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_NO_ACTIVE_WAIT, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, false));
    truing_wait_prompt_t p = apply_prompt(1u, 0.5f);
    const uint32_t id = truing_wait_issue(&g_c, &p);
    i = confirm_adjust(id);
    i.type = TRUING_INTENT_SUBMIT_RUNOUT;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_WRONG_INTENT_FOR_WAIT, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, false));

    /* Debug channel is explicit and off in the demonstration flow (SPEC §12.5). */
    memset(&i, 0, sizeof(i));
    i.type = TRUING_INTENT_DEBUG;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_DEBUG_DISABLED, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, false));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, true));
    /* Enabled is not enough on its own: a session running (any state but READY) must still
     * refuse DEBUG, so MEASURE_ONCE can never land mid-session (SOLENOID_CAMPAIGN_PLAN.md). */
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &g_c, &i, true));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, truing_intent_admissible(TRUING_STATE_MEASURE_SPOKE_TENSION, &g_c, &i, true));

    i.type = TRUING_INTENT_UNSET;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_UNKNOWN, truing_intent_admissible(TRUING_STATE_READY, &g_c, &i, true));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_UNKNOWN, truing_intent_admissible(TRUING_STATE_READY, &g_c, NULL, true));
    TEST_ASSERT_EQUAL_STRING("REJECT_STALE_INTENT", truing_intent_verdict_str(TRUING_INTENT_REJECT_STALE_INTENT));
    TEST_ASSERT_EQUAL_STRING("POSITION_TO_RIM_ANGLE", truing_wait_kind_str(TRUING_WAIT_POSITION_TO_RIM_ANGLE));
}

/* ---- MEASURE_ONCE arg layout (truing/debug_code.h) -----------------------------------------
 * A bare spoke id must stay exactly what it always was; the two campaign overrides ride in bits
 * that used to be out of range, so no old caller changes meaning. */
static void test_measure_once_bare_spoke_id_is_unchanged(void)
{
    for (int32_t spoke = 0; spoke < 36; spoke++) {
        truing_measure_once_args_t a;
        TEST_ASSERT_TRUE(truing_measure_once_unpack(spoke, &a));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)spoke, a.spoke_id);
        TEST_ASSERT_FALSE(a.no_fire);
        TEST_ASSERT_EQUAL_UINT16(0u, a.pulse_ms);
    }
}

static void test_measure_once_pack_and_unpack_round_trip(void)
{
    const truing_measure_once_args_t cases[] = {
        {5u, false, 0u}, {31u, true, 0u}, {2u, false, 40u}, {3u, false, 85u}, {0u, false, 1000u},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        truing_measure_once_args_t back;
        TEST_ASSERT_TRUE(truing_measure_once_unpack(truing_measure_once_pack(&cases[i]), &back));
        TEST_ASSERT_EQUAL_UINT8(cases[i].spoke_id, back.spoke_id);
        TEST_ASSERT_EQUAL(cases[i].no_fire, back.no_fire);
        TEST_ASSERT_EQUAL_UINT16(cases[i].pulse_ms, back.pulse_ms);
    }
    /* the layout itself, since a Python tool packs the same bits: spoke 3, 60 ms */
    const truing_measure_once_args_t sixty = {3u, false, 60u};
    TEST_ASSERT_EQUAL_INT32((60 << 16) | 3, truing_measure_once_pack(&sixty));
    const truing_measure_once_args_t ctl = {2u, true, 0u};
    TEST_ASSERT_EQUAL_INT32(0x102, truing_measure_once_pack(&ctl));
}

static void test_measure_once_unpack_refuses_what_cannot_be_meant(void)
{
    truing_measure_once_args_t a = {77u, true, 77u};   /* must be left alone on refusal */
    TEST_ASSERT_FALSE(truing_measure_once_unpack(-1, &a));                 /* negative */
    TEST_ASSERT_FALSE(truing_measure_once_unpack(0x200 | 3, &a));          /* reserved bit 9 */
    TEST_ASSERT_FALSE(truing_measure_once_unpack(0x8000 | 3, &a));         /* reserved bit 15 */
    TEST_ASSERT_FALSE(truing_measure_once_unpack(0x100 | (40 << 16), &a)); /* no_fire + a pulse width */
    TEST_ASSERT_FALSE(truing_measure_once_unpack((1001 << 16) | 3, &a));   /* past the stuck-actuator bound */
    TEST_ASSERT_FALSE(truing_measure_once_unpack(3, NULL));
    TEST_ASSERT_EQUAL_UINT8(77u, a.spoke_id);
    TEST_ASSERT_TRUE(a.no_fire);
    TEST_ASSERT_EQUAL_UINT16(77u, a.pulse_ms);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_measure_once_bare_spoke_id_is_unchanged);
    RUN_TEST(test_measure_once_pack_and_unpack_round_trip);
    RUN_TEST(test_measure_once_unpack_refuses_what_cannot_be_meant);
    RUN_TEST(test_wait_ids_are_monotonic_and_prompts_are_typed);
    RUN_TEST(test_positioning_prompt_must_name_a_station);
    RUN_TEST(test_wait_match_rules);
    RUN_TEST(test_spec_7_3_duplicate_confirmation_scenario);
    RUN_TEST(test_per_intent_admissibility);
    return UNITY_END();
}
