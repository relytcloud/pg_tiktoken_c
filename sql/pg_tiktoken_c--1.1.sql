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
--   input_text    – text to split
--   chunk_size    – maximum tokens per chunk (must be > 0)
--   chunk_overlap – tokens shared between consecutive chunks (0 <= overlap < chunk_size)
--   encoding      – tiktoken encoding or model alias (default: cl100k_base)
--
-- Algorithm: binary search over character positions to find the rightmost
-- boundary where tiktoken_count <= chunk_size.  For overlap, a second binary
-- search locates the latest start position that still yields >= chunk_overlap
-- tokens from the end of the previous chunk.
CREATE FUNCTION chunk_text(
    input_text    text,
    chunk_size    int,
    chunk_overlap int  DEFAULT 0,
    encoding      text DEFAULT 'cl100k_base'
) RETURNS text[]
LANGUAGE plpgsql IMMUTABLE PARALLEL SAFE
AS $$
DECLARE
    result       text[]  := '{}';
    text_len     int     := length(input_text);
    chunk_start  int     := 1;
    chunk_end    int;
    next_start   int;
    lo           int;
    hi           int;
    mid          int;
BEGIN
    IF chunk_size <= 0 THEN
        RAISE EXCEPTION 'chunk_size must be > 0, got %', chunk_size;
    END IF;
    IF chunk_overlap < 0 OR chunk_overlap >= chunk_size THEN
        RAISE EXCEPTION
            'chunk_overlap must satisfy 0 <= chunk_overlap < chunk_size, got % and %',
            chunk_overlap, chunk_size;
    END IF;
    IF input_text IS NULL OR text_len = 0 THEN
        RETURN result;
    END IF;

    WHILE chunk_start <= text_len LOOP

        -- ── Locate chunk_end ────────────────────────────────────────────────
        -- Rightmost character position (>= chunk_start) where the substring
        -- [chunk_start .. pos] has tiktoken_count <= chunk_size.
        IF tiktoken_count(encoding, substr(input_text, chunk_start)) <= chunk_size THEN
            chunk_end := text_len;
        ELSE
            lo := chunk_start;
            hi := text_len;
            WHILE lo < hi LOOP
                mid := (lo + hi + 1) / 2;
                IF tiktoken_count(encoding,
                       substr(input_text, chunk_start, mid - chunk_start + 1)
                   ) <= chunk_size
                THEN
                    lo := mid;
                ELSE
                    hi := mid - 1;
                END IF;
            END LOOP;
            chunk_end := GREATEST(lo, chunk_start);
        END IF;

        result := result || substr(input_text, chunk_start, chunk_end - chunk_start + 1);

        EXIT WHEN chunk_end >= text_len;

        -- ── Locate next chunk_start (overlap) ───────────────────────────────
        IF chunk_overlap = 0 THEN
            next_start := chunk_end + 1;
        ELSE
            -- Find the largest start P in [chunk_start, chunk_end] where
            -- tiktoken_count(text[P .. chunk_end]) >= chunk_overlap.
            -- As P increases the substring shrinks, so count decreases.
            lo := chunk_start;
            hi := chunk_end;
            WHILE lo < hi LOOP
                mid := (lo + hi + 1) / 2;
                IF tiktoken_count(encoding,
                       substr(input_text, mid, chunk_end - mid + 1)
                   ) >= chunk_overlap
                THEN
                    lo := mid;
                ELSE
                    hi := mid - 1;
                END IF;
            END LOOP;
            next_start := lo;
            -- chunk_overlap < chunk_size guarantees forward progress, but guard anyway.
            IF next_start <= chunk_start THEN
                next_start := chunk_start + 1;
            END IF;
        END IF;

        chunk_start := next_start;
    END LOOP;

    RETURN result;
END;
$$;
