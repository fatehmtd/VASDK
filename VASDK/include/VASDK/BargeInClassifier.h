#pragma once

#include <VASDK/vasdk_export.h>

#include <string>

namespace va {
    namespace pipeline {

        /// What speech over the agent is: a back-channel ("mm-hmm", the agent carries on), a real
        /// interruption, or not known yet (a partial still being heard, or no answer from a model).
        enum class BackchannelVerdict { Backchannel, Interrupt, Undecided };

        /**
         * @brief What a BargeInClassifier is told about speech heard while the agent speaks or thinks.
         */
        struct BargeInContext {
            std::string heard;               ///< the user's words so far (finals, then the latest partial)
            bool isFinal = false;            ///< the user stopped and the STT's final is in
            float confidence = 1.0f;         ///< the STT confidence of the final; 1 when deciding on a partial
            /// true: no reply has played yet (THINKING). The question is then whether the words add to
            /// or change the request (Interrupt) or are an acknowledgement or noise (Backchannel).
            bool agentIsThinking = false;
            std::string agentReply;          ///< the words of the reply being spoken; empty while thinking
            std::string lastUserTurn;        ///< the user's previous turn: the request being answered
            std::string conversationSummary; ///< what was said before, in short; empty when none is kept
        };

        /**
         * @brief Decides, with the context of the conversation, whether speech over the agent is a
         *        back-channel or a real interruption (AgentStateProcessor, BackchannelConfig::decisionMode).
         *
         * classify() blocks: AgentStateProcessor calls it on a worker thread of its own, one call at a
         * time, never on the pipeline's thread. It must be thread-safe, since one object may serve
         * several sessions. Undecided means no answer: the word rules decide instead.
         * LlmBargeInClassifier (llm/LlmBargeInClassifier.h) asks an LLM.
         */
        class VASDK_EXPORT BargeInClassifier {
        public:
            virtual ~BargeInClassifier() = default;
            virtual std::string name() const = 0;
            virtual BackchannelVerdict classify(const BargeInContext& context) const = 0;
        };

    } // namespace pipeline
} // namespace va
