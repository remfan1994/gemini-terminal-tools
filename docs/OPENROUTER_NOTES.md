# OpenRouter notes

ttychatter is OpenRouter-first as of the 0.11.0 cleanup pass.

## Authentication

OpenRouter uses Bearer-token authentication:

```text
Authorization: Bearer <api-key>
```

Supported ttychatter key sources:

1. `OPENROUTER_API_KEY`
2. `TTYCHATTER_API_KEY`
3. `OPENROUTER_API_KEY=` or `API_KEY=` in the config file

## Model catalog

The model catalog endpoint is:

```text
GET https://openrouter.ai/api/v1/models
```

The model catalog is the correct source for model IDs, context lengths, descriptions, router-like IDs, and pricing/capability metadata when available.

## Chat completions

The chat endpoint is:

```text
POST https://openrouter.ai/api/v1/chat/completions
```

The request shape used by ttychatter is OpenAI-compatible:

```json
{
  "model": "openrouter/auto",
  "messages": [
    {"role": "user", "content": "message text"}
  ]
}
```

## Routers and fixed models

OpenRouter may expose fixed model IDs and router-like IDs.

Examples:

```text
openrouter/auto
openrouter/free
```

Do not treat routers as equivalent to fixed underlying models in documentation or future UI. They are user-selectable model identifiers, but they may route dynamically.
