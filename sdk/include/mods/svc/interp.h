#pragma once

#include <mods/api.h>

#ifdef __cplusplus
#include <mods/service.hpp>
#endif

#define INTERP_SERVICE_ID    DUSKLIGHT_SERVICE_ID_PREFIX "interp"
#define INTERP_SERVICE_MAJOR 1u
#define INTERP_SERVICE_MINOR 0u

// This is the same type as Mtx in the Dolphin types.
/**
 * A row-major 3x4 (3 rows, 4 columns) matrix for usage in the interpolation system.
 */
typedef float InterpMtx[3][4];

/**
 * Defines APIs for interpolating render data between multiple simulation frames.
 */
typedef struct InterpService {
    ServiceHeader header;

    /*
     * Matrix API.
     *
     * Using this API is pretty simple: you "record" a matrix during sim frames,
     * then later look up the interpolated "replacement" matrix during rendering.
     *
     * A matrix is identified between frames via an arbitrary pointer. While this is typically
     * the address of the matrix itself, it can be any unique pointer-sized value.
     * You should "unregister" a matrix key with @ref forget_mtx when you are done with it.
     *
     * Note: when working with actors, the draw callback is called from simulation.
     * You should access the interpolated matrices from the packet callback instead.
     */

    /**
     * Record a matrix for interpolation, with an arbitrary key to identify it.
     *
     * @remarks Recorded keys should be unregistered with @ref forget_mtx to avoid memory leaks.
     *
     * @param mod Your mod's context.
     * @param matrix The matrix value to record.
     * @param key The key value used to identify the recorded matrix. Cannot be null.
     */
    ModResult (*record_mtx_keyed)(ModContext* mod, InterpMtx matrix, void const* key);

    /**
     * Record a matrix for interpolation.
     *
     * @remarks This is effectively equivalent to calling @ref record_mtx_keyed with the matrix
     *      as both arguments.
     * @remarks Recorded keys should be unregistered with @ref forget_mtx to avoid memory leaks.
     *
     * @param mod Your mod's context.
     * @param matrix The matrix value to record. Its address is also used as a key to identify it. Cannot be null.
     */
    ModResult (*record_mtx)(ModContext* mod, InterpMtx matrix);

    /**
     * Forgets an interpolated matrix that was previously recorded.
     *
     * @remarks This API does not cause an error if the key is not known.
     */
    ModResult (*forget_mtx)(ModContext* mod, void const* key);

    /**
     * Look up the interpolated replacement for a matrix that was previously recorded.
     *
     * @remarks It is legal to look up replacements for matrices that were registered by other
     *      mods or base game code.
     *
     * @param key Key to look up the replacement for.
     * @param out Pointer that will receive the interpolated replacement matrix.
     *     Not written if the operation fails.
     *
     * @returns false if the matrix is unknown.
     */
    bool (*lookup_replacement_mtx)(void const* key, InterpMtx out);
} InterpService;

MOD_DECLARE_SERVICE(InterpService, svc_interp, INTERP_SERVICE_ID, INTERP_SERVICE_MAJOR,
    INTERP_SERVICE_MINOR);
