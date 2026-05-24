# OpenRouter notes

This release moves the primary ncurses client to OpenRouter's chat-completions API and model-list endpoint. The model browser uses cached metadata and explicit refresh rather than automatic network access.

OpenRouter docs referenced during implementation:

- https://openrouter.ai/docs/quickstart
- https://openrouter.ai/docs/api/api-reference/chat/send-chat-completion-request
- https://openrouter.ai/docs/api/api-reference/models/get-models
