"""Explicit completion declarations for deterministic, finished test answers.

Fixtures call this only after choosing their scenario's reply. Tool action
arrays remain actions; a completion-review turn wraps the intended answer.
Tests of premature completion deliberately do not use this helper.
"""
import json


def reviewed_answer(request, answer):
    system = request['messages'][0]['content']
    if 'COMPLETION REVIEW PROTOCOL:' not in system:
        return answer
    try:
        actions = json.loads(answer)
        if isinstance(actions, list) and actions and all(
                isinstance(action, dict) and 'tool' in action for action in actions):
            return answer
    except (ValueError, TypeError):
        pass
    return json.dumps([{'tool': 'complete_task', 'args': {
        'status': 'complete', 'requires_tools': False, 'answer': answer}}])
