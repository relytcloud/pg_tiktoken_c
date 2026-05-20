\echo Use "CREATE EXTENSION pg_tiktoken_c" to load this file. \quit

-- Returns an array of token IDs for the given text using the specified encoding.
-- encoding_selector: encoding name (cl100k_base, o200k_base, r50k_base, p50k_base, p50k_edit)
--                    or model alias (gpt-3.5-turbo, gpt-4, gpt-4o, text-davinci-003, gpt2, …)
CREATE FUNCTION tiktoken_encode(encoding_selector text, content text)
    RETURNS bigint[]
    AS 'MODULE_PATHNAME', 'tiktoken_encode'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Returns the number of tokens for the given text using the specified encoding.
CREATE FUNCTION tiktoken_count(encoding_selector text, content text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tiktoken_count'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Splits input_text into overlapping token-bounded chunks.
--
-- Parameters:
--   input_text    – text to split; NULL or empty returns '{}'
--   chunk_size    – maximum tokens per chunk (must be > 0)
--   chunk_overlap – tokens shared between consecutive chunks (0 <= overlap < chunk_size)
--   encoding      – tiktoken encoding name or model alias (default: cl100k_base)
--
-- Implemented in C: tokenises the text once, then greedy-packs PCRE2
-- word-pieces into chunks and walks backward to locate the overlap window —
-- no repeated re-tokenisation, no SQL function-call overhead.
CREATE FUNCTION chunk_text(
    input_text    text,
    chunk_size    int,
    chunk_overlap int  DEFAULT 0,
    encoding      text DEFAULT 'cl100k_base'
) RETURNS text[]
    AS 'MODULE_PATHNAME', 'chunk_text'
    LANGUAGE C CALLED ON NULL INPUT IMMUTABLE PARALLEL SAFE;
