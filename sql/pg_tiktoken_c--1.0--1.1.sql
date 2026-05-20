\echo Use "ALTER EXTENSION pg_tiktoken_c UPDATE TO '1.1'" to load this file. \quit

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
