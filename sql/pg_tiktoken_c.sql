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
