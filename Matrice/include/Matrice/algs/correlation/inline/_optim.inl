/*********************************************************************
This file is part of Matrice, an effcient and elegant C++ library.
Copyright(C) 2018-2019, Zhilong(Dgelom) Su, all rights reserved.

This program is free software : you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.If not, see <http://www.gnu.org/licenses/>.
***********************************************************************/
#pragma once

#include "../_optim.h"
#include "../../../arch/ixpacket.h"

MATRICE_ALGS_BEGIN _DETAIL_BEGIN namespace corr {

// \retrieve border size for each interpolation alg.
template<> struct _Corr_border_size<_TAG bicspl_tag> {
	static constexpr auto lower = 1, upper = 2;
};
template<> struct _Corr_border_size<_TAG biqspl_tag> {
	static constexpr auto lower = 2, upper = 3;
};
template<> struct _Corr_border_size<_TAG bisspl_tag> {
	static constexpr auto lower = 3, upper = 4;
};

// \specializations of parameter update strategy.
template<> struct _Param_update_strategy<_Alg_icgn<1>> {
	template<typename _Ty>
	static MATRICE_GLOBAL_FINL auto& eval(_Ty& x, const _Ty& y) {
		constexpr auto _One = one<typename _Ty::value_t>;

		const auto y1p1 = _One + y[1], y5p1 = _One + y[5];
		const auto _Inv_of_det = _One / (y1p1*y5p1 - y[2]*y[4]);
		const auto a = y[2] * y[3] - y[0] * y5p1;
		const auto b = y[0] * y[4] - y[3] * y1p1;

		const auto x1p1 = _One + x[1], x5p1 = _One + x[5];
		const auto u  = _Inv_of_det*(x1p1*a    + x[2]*b) + x[0];
		const auto ux = _Inv_of_det*(x1p1*y5p1 - x[2]*y[4]) - _One;
		const auto uy = _Inv_of_det*(x[2]*y1p1 - y[2]*x1p1);
		const auto v  = _Inv_of_det*(x[4]*a    + x5p1*b) + x[3];
		const auto vx = _Inv_of_det*(x[4]*y5p1 - y[4]*x5p1);
		const auto vy = _Inv_of_det*(x5p1*y1p1 + x[4]*y[2]) - _One;

		x[0] = u, x[1] = ux, x[2] = uy, x[3] = v, x[4] = vx, x[5] = vy;

		return (x);
	}
};

///<base class implementation>
template<typename _Derived> MATRICE_HOST_INL 
auto& _Corr_optim_base<_Derived>::_Cond() {
	// \sent eval.s of Jacobian and Hessian to background.
	auto _J = std::async(std::launch::async, [&] {
		_MyJaco = static_cast<_Derived*>(this)->_Diff();
#if MATRICE_MATH_KERNEL==MATRICE_USE_NAT
		_Myhess = _MyJaco.t().mul(_MyJaco);
#else
		_Myhess = _MyJaco.inplace_mul<transp::Y>(_MyJaco);
#endif
	});

	// \create buf.s to hold reference and current patchs, 
	// and the differences between them.
	_Myref.create(_Mysize, _Mysize, zero<value_type>);
	_Mycur.create(_Mysize, _Mysize);
	_Mydiff.create(sqr(_Mysize), 1);

	// \fill reference image patch.
	const auto& _Data = _Myref_ptr->data();
	const auto[_Rows, _Cols] = _Data.shape();
	const auto[_L, _R, _U, _D] = _Myopt.range<true>(_Mypos);

	auto _Mean = zero<value_type>;
	if (_Mypos.x == floor(_Mypos.x) && _Mypos.y == floor(_Mypos.y)) {
		for (auto y = _U; y < _D; ++y) {
			if (y >= 0 && y < _Rows) {
				const auto _Dp = _Data[y]; auto _Rp = _Myref[y - _U];
				for (auto x = _L; x < _R; ++x) {
					if (x >= 0 && x < _Cols)
						_Mean += _Rp[x - _L] = _Dp[x];
				}
			}
		}
	} else {
		for (auto y = _U; y < _D; ++y) {
			if (y >= 0 && y < _Rows) {
				auto _Rp = _Myref[y - _U];
				for (auto x = _L; x < _R; ++x) {
					if (x >= 0 && x < _Cols)
						_Mean += _Rp[x - _L] = (*_Myref_ptr)(x, y);
				}
			}
		}
	}

	// \zero mean normalization.
	_Myref = _Myref - (_Mean /= _Mysize*_Mysize);
	const auto _Issd = one<value_type> / sqrt(sqr(_Myref).sum());
	_Myref = _Myref * _Issd;

	// \Hessian matrix computation.
	if (_J.valid()) _J.get();
	_Myhess = _Myhess * _Issd;
	_Mysolver.forward();
	_Myhess = _Myhess + matrix_fixed::diag(value_type(0.70));

	return (_Myref);
#undef _IF_WITHIN_RANGE
}

template<typename _Derived> MATRICE_HOST_INL 
auto& _Corr_optim_base<_Derived>::_Warp(const param_type& _Pars) {
#define _IF_WITHIN_RANGE(_OP) \
	if (y-_Mytraits::border_size::lower>=0 && \
		 x-_Mytraits::border_size::lower>=0 && \
		 y+_Mytraits::border_size::upper<_Rows && \
		 x+_Mytraits::border_size::upper<_Cols) \
		_OP; \
   else _Mycur(r,c) = zero<value_type>;

	const auto [_Rows, _Cols] = (*_Mycur_ptr)().shape();
	const auto _Radius = static_cast<index_t>(_Myopt._Radius);
	const auto &u = _Pars[0],    &v = _Pars[3];
	const auto &dudx = _Pars[1], &dudy = _Pars[2];
	const auto &dvdx = _Pars[4], &dvdy = _Pars[5];
	const auto _Cur_x = _Mypos[0] + u, _Cur_y = _Mypos[1] + v;

	auto _Mean = zero<value_type>;
	const auto dvdyp1 = one<value_type> + _Pars[5];
	const auto dudxp1 = one<value_type> + _Pars[1];
	for (diff_t j = -_Radius, r = 0; j <= _Radius; ++j, ++r) {
		const auto dy = static_cast<value_type>(j);
		const auto tx = _Cur_x + _Pars[2] * dy;
		const auto ty = _Cur_y + dvdyp1 * dy;
		auto _Rc = _Mycur[r];
		for (diff_t i = -_Radius, c = 0; i <= _Radius; ++i, ++c) {
			const auto dx = static_cast<value_type>(i);
			const auto x = dudxp1 * dx + tx, y = _Pars[4] * dx + ty;
			_IF_WITHIN_RANGE(_Mean += _Rc[c] = (*_Mycur_ptr)(x,y));
		}
	}

	_Mycur = _Mycur - (_Mean/=_Mysize*_Mysize);
	const auto _Issd = one<value_type>/sqrt(sqr(_Mycur).sum());
	_Mycur = _Mycur * _Issd;

	return (_Mycur);
#undef _IF_WITHIN_RANGE
}
///</base class implementation>

///<derived class implementation>
// \specialization of diff. to eval. Jacobian contributions.
template<typename _Ty, typename _Itag> MATRICE_HOST_INL
auto& _Corr_solver_impl<_Ty, _Itag, _Alg_icgn<1>>::_Diff() {
	const auto& _Size = _Mybase::_Mysize;
	const auto[_L, _R, _U, _D] = _Mybase::_Myopt.range<true>(_Mybase::_Mypos);
	const auto _Off = -static_cast<value_type>(_Mybase::_Myopt._Radius);

	_Mybase::_MyJaco.create(sqr(_Size), _Mybase::DOF);
	for (index_t iy = _U, j = 0; iy < _D; ++iy, ++j) {
		const auto dy = _Off + j, y = _Mybase::_Mypos.y + dy;
		for (index_t ix = _L, i = 0; ix < _R; ++ix, ++i) {
			const auto dx = _Off + i, x = _Mybase::_Mypos.x + dx;
			const auto[dfdx, dfdy] = _Mybase::_Myref_ptr->grad({ x, y });

			auto p = _Mybase::_MyJaco[j * _Size + i];
			p[0] = dfdx, p[1] = dfdx * dx, p[2] = dfdx * dy;
			p[3] = dfdy, p[4] = dfdy * dx, p[5] = dfdy * dy;
		}
	}

	return (_Mybase::_MyJaco);
}

// \specialization for solving IC-GN normal equations.
template<typename _Ty, typename _Itag> MATRICE_HOST_INL
auto _Corr_solver_impl<_Ty,_Itag,_Alg_icgn<1>>::_Solve(param_type& _P) {
	// \warp current image patch.
	_Mybase::_Mycur = _Mybase::_Warp(_P);

	// \error map
	_Mybase::_Mydiff = _Mybase::_Mycur-_Mybase::_Myref;
#ifdef MATRICE_DEBUG
	typename _Mybase::matrix_type
		_Diff(_Mybase::_Mycur.shape(), _Mybase::_Mydiff.data());
#endif // _DEBUG

	// \steepest descent param. update
	param_type _Sdp = _Mybase::_MyJaco.t().mul(_Mybase::_Mydiff);

	// \solve update to the warp parameter vector.
	_Sdp = _Mybase::_Mysolver.backward(_Sdp);
	
	// \inverse composition to update param.
	_P = _Mybase::update_strategy::eval(_P, _Sdp);

	// \report least square correlation coeff. and param. error.
	return std::make_tuple(sqr(_Mybase::_Mydiff).sum(), _Sdp.dot(_Sdp));
}
///</derived class implementation>

_DETAIL_END } MATRICE_ALGS_END