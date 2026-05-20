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

-- Table-valued wrapper around chunk_text().
-- Returns one row per chunk with its 0-based index and token count.
-- Equivalent to:
--   SELECT ... FROM unnest(chunk_text(...)) WITH ORDINALITY ...
-- but more ergonomic for queries that join, filter, or aggregate over chunks.
--
-- Columns:
--   chunk_index  – 0-based position of the chunk in the sequence
--   chunk        – chunk text
--   token_count  – exact token count of this chunk for the given encoding
CREATE FUNCTION chunk_text_table(
    input_text    text,
    chunk_size    int,
    chunk_overlap int  DEFAULT 0,
    encoding      text DEFAULT 'cl100k_base'
) RETURNS TABLE (
    chunk_index  int,
    chunk        text,
    token_count  bigint
)
LANGUAGE SQL IMMUTABLE PARALLEL SAFE AS $$
    SELECT (ord - 1)::int,
           c,
           tiktoken_count(encoding, c)
    FROM   unnest(chunk_text(input_text, chunk_size, chunk_overlap, encoding))
               WITH ORDINALITY AS t(c, ord);
$$;
