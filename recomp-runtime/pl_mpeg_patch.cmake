# Patches pl_mpeg for the TMPGEnc-encoded Sofdec movies, matching FFmpeg's
# mpeg12dec.c: DC size codes 9-11 and an intra DC precision the caller sets.
function(_recomp_pl_mpeg_replace var from to expected)
    string(LENGTH "${${var}}" before)
    string(REPLACE "${from}" "" removed "${${var}}")
    string(LENGTH "${removed}" after)
    string(LENGTH "${from}" size)
    math(EXPR found "(${before} - ${after}) / ${size}")
    if(NOT found EQUAL expected)
        message(FATAL_ERROR "pl_mpeg patch expected ${expected} of '${from}', found ${found}")
    endif()
    string(REPLACE "${from}" "${to}" result "${${var}}")
    set(${var} "${result}" PARENT_SCOPE)
endfunction()

function(recomp_patch_pl_mpeg source destination)
    file(READ "${source}" text)
    _recomp_pl_mpeg_replace(text [=[	{       0,    8}, {      -1,    0},  //   8: 1111 11x]=] [=[	{       0,    8}, {  9 << 1,    0},  //   8: 1111 11x
	{       0,    9}, { 10 << 1,    0},  //   9: 1111 111x
	{       0,   10}, {       0,   11},  //  10: 1111 1111 x]=] 1)
    _recomp_pl_mpeg_replace(text [=[	{       0,    8}, {      -1,    0},  //   8: 1111 111x]=] [=[	{       0,    8}, {  9 << 1,    0},  //   8: 1111 111x
	{       0,    9}, { 10 << 1,    0},  //   9: 1111 1111 x
	{       0,   10}, {       0,   11},  //  10: 1111 1111 1x]=] 1)
    _recomp_pl_mpeg_replace(text [=[	int dc_predictor[3];]=] [=[	int dc_predictor[3];
	int intra_dc_precision;]=] 1)
    _recomp_pl_mpeg_replace(text [=[] = 128;]=] [=[] = 128 << self->intra_dc_precision;]=] 9)
    _recomp_pl_mpeg_replace(text [=[block_data[0] <<= (3 + 5);]=] [=[block_data[0] <<= (3 - self->intra_dc_precision + 5);]=] 1)
    file(WRITE "${destination}" "${text}")
endfunction()
