LOAD 'MODULE_PATHNAME';

CREATE FUNCTION sample_prob() RETURNS FLOAT8
LANGUAGE 'c' VOLATILE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

/* running_sample_size is used to update the ni */
CREATE FUNCTION running_sample_size() RETURNS FLOAT8
LANGUAGE 'c' VOLATILE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION running_sample_budget() RETURNS FLOAT8
LANGUAGE 'c' VOLATILE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION running_state_id() RETURNS FLOAT8
LANGUAGE 'c' VOLATILE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION inv_sample_prob() RETURNS FLOAT8
LANGUAGE 'c' VOLATILE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION erf_inv(FLOAT8) RETURNS FLOAT8
LANGUAGE 'c' IMMUTABLE STRICT PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_erf_inv_pg';

CREATE FUNCTION swr(INTERNAL) RETURNS TSM_HANDLER
LANGUAGE 'c' VOLATILE AS 'MODULE_PATHNAME', 'aqp_tablesample_swr_tsm_handler';

CREATE FUNCTION pswr(INTERNAL) RETURNS TSM_HANDLER
LANGUAGE 'c' VOLATILE AS 'MODULE_PATHNAME', 'aqp_tablesample_pswr_tsm_handler';

CREATE FUNCTION dummy_f8_func_f8(FLOAT8) RETURNS FLOAT8
LANGUAGE 'c' PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION dummy_f8_func_f8_f8(FLOAT8, FLOAT8) RETURNS FLOAT8
LANGUAGE 'c' PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION dummy_f8_func_f8_any(FLOAT8, "any") RETURNS FLOAT8
LANGUAGE 'c' PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION dummy_f8_func_f8_any_f8(FLOAT8, "any", FLOAT8) RETURNS FLOAT8
LANGUAGE 'c' PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE FUNCTION dummy_f8_func_internal(INT8) RETURNS FLOAT8
LANGUAGE 'c' PARALLEL SAFE AS 'MODULE_PATHNAME', 'aqp_dummy_func';

CREATE AGGREGATE float8_accum(FLOAT8) (
    SFUNC = float8_accum,
    STYPE = FLOAT8[],
    INITCOND = '{0, 0, 0}'
);

CREATE FUNCTION clt_half_ci_finalfunc(FLOAT8[], FLOAT8, FLOAT8)
RETURNS FLOAT8
LANGUAGE 'c' IMMUTABLE STRICT PARALLEL SAFE
AS 'MODULE_PATHNAME', 'aqp_clt_half_ci_finalfunc';

CREATE AGGREGATE approx_sum("any") (
    SFUNC = dummy_f8_func_f8_any,
    STYPE = FLOAT8
);

CREATE AGGREGATE approx_sum_half_ci("any", FLOAT8) (
    SFUNC = dummy_f8_func_f8_any_f8,
    STYPE = FLOAT8
);

CREATE AGGREGATE approx_count(*) (
    SFUNC = dummy_f8_func_f8,
    STYPE = FLOAT8
);

CREATE AGGREGATE approx_count_star_half_ci(FLOAT8) (
    SFUNC = dummy_f8_func_f8_f8,
    STYPE = FLOAT8
);

CREATE AGGREGATE approx_count("any") (
    SFUNC = dummy_f8_func_f8_any,
    STYPE = FLOAT8
);

CREATE AGGREGATE approx_count_half_ci("any", FLOAT8) (
    SFUNC = dummy_f8_func_f8_any_f8,
    STYPE = FLOAT8
);


CREATE FUNCTION progressive_clt_half_ci_finalfunc(FLOAT8[], FLOAT8, FLOAT8) 
RETURNS FLOAT8
LANGUAGE 'c' IMMUTABLE STRICT PARALLEL SAFE
AS 'MODULE_PATHNAME', 'aqp_progressive_clt_half_ci_finalfunc';

CREATE FUNCTION progressive_float8_accum(FLOAT8[], FLOAT8, FLOAT8, FLOAT8) RETURNS FLOAT8[]
LANGUAGE 'c' VOLATILE AS 'MODULE_PATHNAME', 'aqp_progressive_float8_accum';

CREATE AGGREGATE progressive_float8_accum(FLOAT8, FLOAT8, FLOAT8) (
    SFUNC = progressive_float8_accum,
    STYPE = FLOAT8[],
    INITCOND = '{0, 0, 0, 0, 0, 0}'
);

CREATE AGGREGATE approx_progressive_count_star_half_ci(FLOAT8) (
    SFUNC = dummy_f8_func_f8_f8,
    STYPE = FLOAT8
);

CREATE FUNCTION approx_sum_internal_accum(INT8, FLOAT8)
RETURNS INT8
LANGUAGE 'c' STRICT IMMUTABLE PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'aqp_approx_sum_internal_accum';

CREATE FUNCTION approx_sum_internal_final(INT8)
RETURNS FLOAT8
LANGUAGE 'c' STRICT IMMUTABLE PARALLEL SAFE
AS 'MODULE_PATHNAME', 'aqp_approx_sum_internal_final';

CREATE FUNCTION approx_sum_clt_half_ci_internal_final(INT8, FLOAT8)
RETURNS FLOAT8
LANGUAGE 'c' STRICT IMMUTABLE PARALLEL SAFE
AS 'MODULE_PATHNAME', 'aqp_approx_sum_clt_half_ci_internal_final';

CREATE AGGREGATE approx_sum_internal(FLOAT8) (
    SFUNC = approx_sum_internal_accum,
    STYPE = INT8,
    INITCOND = -1, -- MAX_UINT64
    FINALFUNC = dummy_f8_func_internal,
    FINALFUNC_MODIFY = READ_ONLY
);

