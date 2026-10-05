#pragma once

#include <cstring>

#include "interp.h"

namespace mods::interp {

/**
 * RAII wrapper for interpolation matrices.
 *
 * @remarks This is a simple wrapper around a @ref InterpMtx.
 *      The destructor automatically calls @ref forget_mtx, while the assignment operators
 *      automatically call @ref record.
 */
struct InterpMatrix {
    InterpMtx mtx{};

    InterpMatrix() = default;

    explicit(false) InterpMatrix(InterpMtx const mtx) {
        std::memcpy(&this->mtx, mtx, sizeof(this->mtx));
    }

    InterpMatrix(InterpMatrix const& src) : InterpMatrix(src.mtx) {}

    InterpMatrix& operator=(InterpMatrix const& other) { return operator=(other.mtx); }

    InterpMatrix& operator=(InterpMtx const other) {
        std::memcpy(&this->mtx, other, sizeof(this->mtx));
        record();
        return *this;
    }

    /**
     * Record the current value of the matrix.
     */
    void record() { svc_interp->record_mtx(mod_ctx, mtx); }

    /**
     * Read the interpolated value of the matrix.
     */
    void readInterpolated(InterpMtx& out) const {
        if (!svc_interp->lookup_replacement_mtx(&mtx, out)) {
            std::memcpy(out, mtx, sizeof(mtx));
        }
    }

    ~InterpMatrix() { svc_interp->forget_mtx(mod_ctx, &mtx); }
};

}  // namespace mods::interp
