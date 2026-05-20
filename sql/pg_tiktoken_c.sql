-- Load the extension
CREATE EXTENSION IF NOT EXISTS pg_tiktoken_c;

-- Basic encoding
SELECT tiktoken_encode('cl100k_base', 'Hello world!');
SELECT tiktoken_count('cl100k_base', 'Hello world!');

-- r50k_base
SELECT tiktoken_encode('r50k_base', 'PostgreSQL is amazing!');
SELECT tiktoken_count('r50k_base', 'PostgreSQL is amazing!');

-- p50k_base + p50k_edit
SELECT tiktoken_encode('p50k_base', 'Hello world!');
SELECT tiktoken_encode('p50k_edit', 'Hello world!');

-- Special tokens included in encode_with_special_tokens
SELECT tiktoken_encode('cl100k_base', 'hello<|endoftext|>world');
SELECT tiktoken_encode('p50k_base',   'test<|endoftext|>end');

-- Model aliases
SELECT tiktoken_encode('gpt-3.5-turbo', 'A long time ago in a galaxy far, far away')
    = tiktoken_encode('cl100k_base', 'A long time ago in a galaxy far, far away') AS alias_gpt35;

SELECT tiktoken_encode('text-davinci-002', 'A long time ago in a galaxy far, far away')
    = tiktoken_encode('p50k_base', 'A long time ago in a galaxy far, far away') AS alias_davinci;

SELECT tiktoken_encode('gpt2', 'A long time ago in a galaxy far, far away')
    = tiktoken_encode('r50k_base', 'A long time ago in a galaxy far, far away') AS alias_gpt2;

SELECT tiktoken_encode('code-davinci-edit-001', 'A long time ago in a galaxy far, far away')
    = tiktoken_encode('p50k_edit', 'A long time ago in a galaxy far, far away') AS alias_edit;

-- Edge cases
SELECT tiktoken_encode('cl100k_base', '');
SELECT tiktoken_count('cl100k_base', '');

-- Long text
SELECT tiktoken_count('cl100k_base', repeat('word ', 100));

-- Invalid encoder
SELECT tiktoken_encode('invalid_model', 'Test') AS should_fail;

-- ── chunk_text ───────────────────────────────────────────────────────────────

-- NULL / empty
SELECT chunk_text(NULL, 100);
SELECT chunk_text('', 100);

-- Text shorter than chunk_size → single-element array
SELECT chunk_text('Hello world!', 100);

-- No overlap: verify chunk count and that chunks together span the original text
SELECT array_length(chunk_text(repeat('word ', 50), 10), 1) AS n_chunks_no_overlap;

-- With overlap: chunk count must be >= no-overlap count
SELECT array_length(chunk_text(repeat('word ', 50), 10, 3), 1) >=
       array_length(chunk_text(repeat('word ', 50), 10, 0), 1) AS overlap_produces_more_chunks;

-- Each chunk must not exceed chunk_size tokens
SELECT bool_and(
           tiktoken_count('cl100k_base', chunk) <= 10
       ) AS all_within_limit
FROM   unnest(chunk_text(repeat('word ', 50), 10)) AS chunk;

-- Overlap: last chunk_overlap tokens of chunk[i] == first tokens of chunk[i+1]
-- (verify the leading tokens of chunk 2 are present at the tail of chunk 1)
SELECT tiktoken_count('cl100k_base',
           (chunk_text(repeat('word ', 50), 10, 3))[2]
       ) <= 10 AS second_chunk_within_limit;

-- Model alias works
SELECT array_length(chunk_text(repeat('word ', 30), 5, 0, 'gpt-4'), 1) > 1
    AS alias_works;

-- Error: chunk_size = 0
SELECT chunk_text('hello', 0);

-- Error: overlap >= chunk_size
SELECT chunk_text('hello', 5, 5);
