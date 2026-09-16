package agent

import (
	"context"
	"strings"
	"time"
)

const defaultChatBudget = 14 * time.Second
const timeoutNotice = "部分查询超时或已取消，结果可能不完整，请稍后重试。"

// Chat answers one question.
//
// It does not return an error. Every internal failure - no model, a model that
// times out, a map provider that is down - is turned into a degraded answer,
// because a user who asked a question is better served by a partial, labelled
// result than by a failure they cannot act on.
func (s *Service) Chat(ctx context.Context, message string, conv Context) (result Result) {
	ctx, cancel := context.WithTimeout(ctx, s.budget)
	defer cancel()
	timedOut := false
	defer func() {
		if timedOut || ctx.Err() != nil {
			result.Degraded = true
			result.Reply = strings.TrimSpace(result.Reply + " " + timeoutNotice)
		}
	}()
	if conv.Now.IsZero() {
		conv.Now = s.clock()
	}
	message = strings.TrimSpace(message)

	var (
		degraded     bool
		planned      []Invocation
		llmReachable bool
	)

	// The browser's position arrives in WGS-84 and everything downstream speaks
	// GCJ-02. Converting once, here, is what stops the difference from being
	// applied inconsistently - or not at all - by each caller.
	//
	// When the conversion is unavailable the position is kept and the answer is
	// marked degraded rather than answered without one. The offset is a few
	// hundred metres: enough to reorder two stations that are close together,
	// and far less harmful than dropping the position, which would turn "the
	// nearest three stations" into three arbitrary ones. The flag is what tells
	// the user the answer is approximate.
	if conv.Location != nil && conv.WGS84 {
		if converted, ok := s.normalizePosition(*conv.Location); ok {
			conv.Location = &converted
		} else {
			degraded = true
		}
	}
	conv.WGS84 = false

	llmEnabled := s.llm != nil && s.llm.Available()
	if llmEnabled && ctx.Err() == nil {
		planCtx, stopPlan := context.WithTimeout(ctx, s.planningBudget)
		planned = s.planFromModel(planCtx, message, conv, &llmReachable)
		timedOut = planCtx.Err() != nil
		stopPlan()
		if !llmReachable {
			degraded = true
		}
	}
	if len(planned) == 0 {
		// Either no model, or a model that chose no tool: fall back to the
		// deterministic plan so the client always receives structured data.
		category := poiCategoryFor(message)
		for _, name := range s.PlanTools(message) {
			invocation := Invocation{Name: name}
			if name == toolPoiSearch && category != "" {
				invocation.Arguments = Arguments{"category": category}
			}
			planned = append(planned, invocation)
		}
	}
	if !llmEnabled {
		degraded = true
	}

	planned = s.withAnchor(planned, conv)

	failed, observations := s.execute(ctx, planned, conv, &result, &degraded)
	s.buildActions(&result)

	if llmEnabled && llmReachable && len(observations) > 0 && ctx.Err() == nil {
		if reply, ok := s.compose(ctx, message, conv, observations); ok {
			result.Reply = reply
			result.LLMUsed = true
		} else {
			// The model was reachable for planning but not for wording, which is
			// a degradation like any other.
			degraded = true
		}
	}

	if !result.LLMUsed {
		result.Reply = s.fallbackReply(message, planned, result, failed, llmEnabled && llmReachable)
	}

	// A route computed from a straight line is a degraded answer even when the
	// model wrote the wording around it: the user is about to be told a driving
	// distance that is not a driving distance, and only this flag, the
	// route's own fallback marker and the closing notice say so.
	if result.Route != nil && result.Route.Fallback {
		degraded = true
		if !strings.Contains(result.Reply, mapUnavailableNotice) {
			result.Reply = strings.TrimSpace(result.Reply + " " + mapUnavailableNotice)
		}
	}
	if !conv.HasLocation() && len(result.Stations) == 0 && !result.LLMUsed {
		result.Reply = strings.TrimSpace(result.Reply + " " + noLocationNotice)
	}

	result.Degraded = degraded
	return result
}
