\echo Use "CREATE EXTENSION pg_tiktoken_c" to load this file. \quit

-- Returns an array of token IDs for the given text using the specified encoding.
-- encoding_selector: encoding name (cl100k_base, r50k_base, p50k_base, p50k_edit)
--                    or model alias (gpt-3.5-turbo, gpt-4, text-davinci-003, gpt2, …)
CREATE FUNCTION tiktoken_encode(encoding_selector text, content text)
    RETURNS bigint[]
    AS 'MODULE_PATHNAME', 'tiktoken_encode'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Returns the number of tokens for the given text using the specified encoding.
CREATE FUNCTION tiktoken_count(encoding_selector text, content text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'tiktoken_count'
    LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
