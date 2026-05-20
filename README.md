# pg_tiktoken_c

A PostgreSQL extension that brings OpenAI's [tiktoken](https://github.com/openai/tiktoken) BPE tokenizer into SQL — implemented in pure C for maximum performance.

Use it to count tokens before calling OpenAI APIs, build RAG pipelines that respect context limits, or store token arrays alongside your text data.

## Features

- **Token counting** — know your token budget before hitting the API
- **Token ID arrays** — store or inspect the exact BPE token sequence
- **All major OpenAI encodings** — `cl100k_base`, `o200k_base`, `r50k_base`, `p50k_base`, `p50k_edit`
- **Model name aliases** — pass `'gpt-4'` or `'gpt-3.5-turbo'` directly, no need to remember encoding names
- **Special token support** — correctly handles `<|endoftext|>` and similar control tokens
- **Fast** — encoder state cached per-process in `TopMemoryContext`; BPE loop avoids heap allocation; open-addressing hash table fits L2/L3 cache

## Supported Encodings & Model Aliases

| Encoding | Model aliases |
|---|---|
| `cl100k_base` | `gpt-4`, `gpt-4-*`, `gpt-3.5-turbo`, `gpt-3.5-turbo-*`, `text-embedding-ada-002`, `text-embedding-3-small`, `text-embedding-3-large` |
| `o200k_base` | `gpt-4o`, `gpt-4o-*`, `o1`, `o1-*`, `o3`, `o3-*` |
| `r50k_base` | `gpt2`, `davinci` |
| `p50k_base` | `text-davinci-002`, `text-davinci-003`, `code-davinci-001`, `code-cushman-001` |
| `p50k_edit` | `text-davinci-edit-001`, `code-davinci-edit-001` |

## Quick Start with RelytOne (Recommended)

The easiest way to use `pg_tiktoken_c` is through [RelytOne](https://relytone.data.cloud/auth/signin), a fully managed PostgreSQL cloud service with this extension pre-installed.

### 1. Create an instance

Go to [https://relytone.data.cloud/auth/signin](https://relytone.data.cloud/auth/signin) and sign in. Then:

1. Click **New Instance** in the dashboard
2. Choose an instance type (the extension is available on all tiers)
3. Wait ~30 seconds for provisioning
4. Copy the connection string from the instance details page

### 2. Connect and enable the extension

```sql
CREATE EXTENSION IF NOT EXISTS pg_tiktoken_c;
```

That's it — the vocab data files are pre-loaded and no extra setup is needed.

## Self-Hosted Installation

### Prerequisites

| Requirement | Version |
|---|---|
| PostgreSQL | 13 – 17 |
| PCRE2 | ≥ 10.30 |
| C compiler | GCC / Clang |

Install PCRE2:

```bash
# macOS (Homebrew)
brew install pcre2

# Ubuntu / Debian
sudo apt-get install libpcre2-dev

# RHEL / Rocky
sudo dnf install pcre2-devel
```

### Build & Install

```bash
# Clone the repository
git clone https://github.com/relytcloud/pg_tiktoken_c.git
cd pg_tiktoken_c

# Build and install
make
sudo make install

# Download vocab data files (required once after install)
sudo make download-data
```

The `download-data` target fetches the BPE vocabulary files from `openaipublic.blob.core.windows.net` and installs them into PostgreSQL's `sharedir`. Requires internet access; files total ~10 MB.

### Enable the Extension

```sql
CREATE EXTENSION pg_tiktoken_c;
```

## SQL Reference

### `tiktoken_count(encoding_selector, content)`

Returns the number of tokens in `content` for the given encoding.

```
tiktoken_count(encoding_selector text, content text) → bigint
```

### `tiktoken_encode(encoding_selector, content)`

Returns an array of integer token IDs.

```
tiktoken_encode(encoding_selector text, content text) → bigint[]
```

Both functions are `IMMUTABLE PARALLEL SAFE` and can be used in indexes, generated columns, and parallel queries.

## Usage Examples

### Count tokens before calling the API

```sql
-- Check token budget for GPT-4o
SELECT tiktoken_count('gpt-4o', prompt_text) AS token_count
FROM   my_prompts
WHERE  tiktoken_count('gpt-4o', prompt_text) <= 128000;
```

### Truncate text to a token limit

```sql
-- Keep chunks under 512 tokens for embedding
WITH encoded AS (
    SELECT id,
           body,
           tiktoken_encode('text-embedding-3-small', body) AS tokens
    FROM   documents
)
SELECT id,
       array_length(tokens, 1)                        AS token_count,
       tokens[1:512]                                  AS truncated_tokens
FROM   encoded;
```

### Store token counts as a generated column

```sql
ALTER TABLE articles
    ADD COLUMN token_count bigint
    GENERATED ALWAYS AS (tiktoken_count('cl100k_base', body)) STORED;

-- Query by token range
SELECT title, token_count
FROM   articles
WHERE  token_count BETWEEN 100 AND 500
ORDER  BY token_count;
```

### Special tokens

```sql
SELECT tiktoken_encode('cl100k_base', 'hello<|endoftext|>world');
-- {15339,100257,14957}
```

### Inspect individual token IDs

```sql
SELECT token_id,
       row_number() OVER () - 1 AS position
FROM   unnest(tiktoken_encode('cl100k_base', 'Hello world!')) AS t(token_id);
--  token_id | position
-- ----------+----------
--      9906 |        0
--      1917 |        1
--         0 |        2
```

### Quick reference for common encodings

```sql
SELECT tiktoken_count('cl100k_base',   'Hello world!') AS cl100k,
       tiktoken_count('o200k_base',    'Hello world!') AS o200k,
       tiktoken_count('r50k_base',     'Hello world!') AS r50k,
       tiktoken_count('p50k_base',     'Hello world!') AS p50k;
```

## Running Tests

```bash
make installcheck
```

## Performance Notes

- The encoder (vocab hash table + PCRE2 pattern) is loaded once per PostgreSQL backend and cached for the lifetime of the connection — repeated calls within the same session pay no reload cost.
- The BPE merge loop avoids heap allocation for typical token lengths (< 2 KB word pieces).
- The hash table uses open-addressing with FNV-1a, sized to fit in L2/L3 cache.
- For bulk token counting over large tables, consider using a parallel query or `PARALLEL SAFE` aggregate.

## License

Apache 2.0. See [LICENSE](LICENSE).
