"""Context-based barge-in from Python, through the C API: who decides on speech over the assistant
(barge_in_decision) is set on the session (vasdk.Backchannel, as set_backchannel takes it), and an
agent's pages and parts choose their own, which each reply segment carries.
ctest runs it as vasdk_python_barge_in; the binding comes from conftest.py.
"""

AGENT = """
start_flow: "main"
barge_in_decision: MODEL_WHEN_RULES_UNSURE
flows { name: "main" start_page: "confirm" pages {
  name: "confirm"
  barge_in_decision: MODEL_FOR_EVERY_OVERLAP
  on_enter { say { text: "I have a table for four on Tuesday at 7 pm." } }
  on_enter { say { text: "Shall I confirm it?" barge_in_decision: WORD_RULES_ONLY } }
} }
"""


def test_set_backchannel_takes_the_decision_mode(vasdk):
    assert vasdk.BargeInDecision.MODEL_FOR_EVERY_OVERLAP == 2, "the C API's VASDK_BARGE_IN_MODEL_FOR_EVERY_OVERLAP"
    # What Session.set_backchannel sends: a vasdk.Backchannel, its mode a vasdk.BargeInDecision.
    backchannel = vasdk.Backchannel(barge_in_decision=vasdk.BargeInDecision.MODEL_WHEN_RULES_UNSURE,
                                    barge_in_llm_profile="system1", barge_in_model_timeout_ms=600,
                                    barge_in_while_thinking=True)
    command = vasdk.Command(set_backchannel=backchannel)
    sent = vasdk.Command.FromString(command.SerializeToString()).set_backchannel
    assert sent.barge_in_decision == vasdk.BargeInDecision.MODEL_WHEN_RULES_UNSURE
    assert sent.barge_in_llm_profile == "system1"
    assert sent.barge_in_model_timeout_ms == 600
    assert sent.barge_in_while_thinking


def test_reply_segments_carry_their_part_and_page_choice(vasdk, tmp_path):
    agent_file = tmp_path / "booking.txtpb"
    agent_file.write_text(AGENT)
    reply = vasdk.Agent.load(agent_file).conversation().start()
    assert [segment.text for segment in reply.segments] == ["I have a table for four on Tuesday at 7 pm.", "Shall I confirm it?"]
    assert reply.segments[0].barge_in_decision == vasdk.BargeInDecision.MODEL_FOR_EVERY_OVERLAP, "the page's choice"
    assert reply.segments[1].barge_in_decision == vasdk.BargeInDecision.WORD_RULES_ONLY, "the part's own"
    assert reply.segments[1].HasField("barge_in_decision")
