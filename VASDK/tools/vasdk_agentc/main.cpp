// vasdk_agentc: checks, tests and chats with agent files, with no audio and no LLM.
//   vasdk_agentc lint agent.txtpb                  problems, with where they are; exit 1 on errors
//   vasdk_agentc test agent.txtpb case.txtpb...    plays each FlowTestCase; exit 1 on a failure
//   vasdk_agentc chat agent.txtpb [--session name=value]...
//                                                  talk to it by typing; an empty line is silence
//                                                  (no-input); Ctrl-D quits
//   vasdk_agentc try agent.txtpb [--entity @name] "words"
//                                                  the intent and entities found in the words

// Agents that use steps, page types or envelopes written in C++ need the app's own copy of this
// tool, which registers them: see flow/AgentTool.h.

#include <VASDK/flow/AgentTool.h>

int main(int argc, char** argv) { return va::flow::runAgentTool(argc, argv); }
