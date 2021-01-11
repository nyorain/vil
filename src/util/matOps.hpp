// Copyright (c) 2017-2020 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

/// Various matrix related operations.

#pragma once

#ifndef NYTL_INCLUDE_MAT_OPS
#define NYTL_INCLUDE_MAT_OPS

#include <util/tmpUtil.hpp> // nytl::templatize
#include <util/mat.hpp> // nytl::Mat
#include <util/vecOps.hpp> // nytl::dot

#include <utility> // std::swap
#include <stdexcept> // std::invalid_argument
#include <tuple> // std::tuple
#include <iosfwd> // std::ostream
#include <cmath> // std::fma

namespace nytl {

/// The return type of a LU decomposition.
/// Can then be used by other operations that don't have to decompose
/// the matrix again, like determinant, inverse or luEvaluate (solve
/// an equation).
template<size_t D, typename T>
struct LUDecomposition {
	nytl::Mat<D, D, T> lower;
	nytl::Mat<D, D, T> upper;
	nytl::Mat<D, D, bool> perm; // permutation
	unsigned int sign = 1;
};

/// Prints the given matrix with numerical values to the given ostream.
/// If this function is used, header <ostream> must be included.
/// This function does not implement operator<< since this operator should only implemented
/// for the Vector implementation types. Can be used to implement such an operator (as
/// nytl::Mat does).
/// - valueWidth: The number of characters per matrix value. Can be 0 to not care for it.
/// - rowSpacing The spacing in the beginning of each row.
/// - breakAfter Whether to insert a newline after printing the matrix.
/// There must be an implementation of operator<<(std::ostream&, M::Value).
template<size_t R, size_t C, typename T>
std::ostream& print(std::ostream& ostream, const Mat<R, C, T>& mat,
		unsigned int valueWidth = 8, const char* rowSpacing = "    ",
		bool breakAfter = true) {

	auto& os = templatize<T>(ostream);
	auto org = os.precision();
	os << "{" << "\n";

	// returns the number of digits needed to present the integer part of i
	const auto numberOfDigits = [](double i) {
		return (i < 10 && i >= 0) ? 1 : (i > -10 && i < 0) ? 2 :
			(i > 0) ? std::log10(i) + 1 : std::log10(-i) + 2;
	};

	for(auto r = 0u; r < R; ++r) {
		os << rowSpacing << "(";

		for(auto c = 0u; c < C; c++) {
			if(valueWidth) os.width(valueWidth);
			if(valueWidth) os.precision(valueWidth - numberOfDigits(mat[r][c]) - 1);

			os << mat[r][c];
			if(c != C - 1)
				os << ", ";
		}

		os << ")" << "\n";
	}

	os << "}";
	if(breakAfter) os << "\n";
	if(valueWidth) os.precision(org);
	return os;
}

template<typename T, size_t R, size_t C>
std::ostream& operator<<(std::ostream& os, const Mat<R, C, T>& a) {
	return print(os, a);
}

/// Returns the column with index n of the given matrix.
/// For example: `nytl::mat::row(mat44, 1);` returns the second (index 1) column of a matrix.
template<size_t R, size_t C, typename T>
constexpr auto col(const nytl::Mat<R, C, T>& mat, size_t n) {
	Vec<R, T> ret {};
	for(auto i = 0u; i < R; ++i) {
		ret[i] = mat[i][n];
	}

	return ret;
}

/// Sets the column with index n to the given column.
/// For example: `nytl::mat::col(mat44, 2, vec4);` sets the 3rd column of a matrix.
template<size_t R, size_t C, typename T>
constexpr void col(nytl::Mat<R, C, T>& mat, size_t n, const nytl::Vec<R, T>& col) {
	for(auto i = 0u; i < R; ++i) {
		mat[i][n] = col[i];
	}
}

/// Swaps the row with index n with the row with index i.
/// For example: `nytl::mat::swapRow(mat44, 2, 3);` swaps the 3rd and 4th row
template<size_t R, size_t C, typename T>
constexpr void swapRow(nytl::Mat<R, C, T>& mat, size_t n, size_t i) {
	using std::swap;
	swap(mat[n], mat[i]);
}

/// Swaps the column with index n with the column with index i.
/// For example: `nytl::mat::swapCol(mat44, 2, 3);` swaps the 3rd and 4th column
template<size_t R, size_t C, typename T>
constexpr void swapCol(nytl::Mat<R, C, T>& mat, size_t n, size_t i) {
	using std::swap;
	for(auto r = 0u; r < R; ++r) {
		swap(mat[r][n], mat[r][i]);
	}
}

/// Sets all values of the given matrix to 1.
template<size_t R, size_t C, typename T>
constexpr void one(nytl::Mat<R, C, T>& mat) {
	for(auto r = 0u; r < R; ++r)
		for(auto c = 0u; c < C; ++c)
			mat[r][c] = T{1};
}

/// Returns the trace of a square matrix, i.e. the sum of its diagonal elements
template<size_t D, typename T>
constexpr auto trace(const nytl::Mat<D, D, T>& mat) {
	auto ret = mat[0][0];
	for(auto n = 1u; n < D; ++n) {
		ret += mat[n][n];
	}

	return ret;
}

/// Multiplies all elements in the diagonal of the given non-empty square matrix.
template<size_t D, typename T>
constexpr auto multiplyDiagonal(const nytl::Mat<D, D, T>& mat) {
	auto ret = mat[0][0];
	for(auto n = 1u; n < D; ++n) {
		ret *= mat[n][n];
	}

	return ret;
}

/// Sets the given matrix to the identity matrix.
template<size_t D, typename T>
constexpr void identity(nytl::Mat<D, D, T>& mat) {
	mat = {};
	for(auto n = 0u; n < D; ++n) {
		mat[n][n] = T{1};
	}
}

/// Returns the identity for the given size and precision
template<size_t D, typename T>
constexpr auto identity() {
	Mat<D, D, T> ret {};
	identity(ret);
	return ret;
}

/// Transposes the given matrix.
template<size_t R, size_t C, typename T>
constexpr auto transpose(const nytl::Mat<R, C, T>& mat) {
	nytl::Mat<C, R, T> ret {};
	for(auto r = 0u; r < R; ++r) {
		for(auto c = 0u; c < C; ++c) {
			ret[c][r] = mat[r][c];
		}
	}

	return ret;
}

/// Performs partial pivoting for the given matrix for given position.
/// Finds the largest value in the given column and swaps its row with the given row.
/// Complexity lies within O(n).
/// - row The row of the matrix entry to maximize.
/// - column The column of the matrix entry to maximize.
/// - onlyAfter If this is true, only rows after the given one are considered for swapping.
/// Returns the new value at the given position.
template<size_t R, size_t C, typename T>
constexpr auto pivot(nytl::Mat<R, C, T>& mat, size_t row, size_t column, bool onlyAfter = false) {
	auto maxRow = row;
	for(auto r = onlyAfter ? row + 1 : 0; r < R; ++r) {
		if(std::abs(mat[r][column]) > std::abs(mat[maxRow][column])) {
			maxRow = r;
		}
	}

	if(maxRow != row) {
		swapRow(mat, row, maxRow);
	}

	return mat[row][column];
}

/// Brings the given matrix into the row echolon form (ref).
/// Implements the first step of the Gaussian elimination algorithm for a given matrix.
/// The given matrix does not have to fulfill any requirements.
/// Does directly modify the matrix. For a version that operates on a copy, see
/// rowEcholonCopy.
/// Complexity Lies within O(n^3) where n is the number of rows/cols of the given matrix.
/// NOTE: This operation divides by values from the matrix so it must have a type does
/// correctly implement division over the desired field (e.g. integer matrices will result
/// in quiet rounding errors here).
template<size_t R, size_t C, typename T>
constexpr void rowEcholon(nytl::Mat<R, C, T>& mat) {

	// Start with the topleft element and go one right in each step.
	// We only go one down if we could eliminate the current column with the current row.
	for(auto r = 0u, c = 0u; r < R && c < C; ++c) {
		// maximize the current pivot. Only consider rows after the current one.
		// go to the next column (continue without increasing the row) if the pivot is zero
		if(pivot(mat, r, c, true) == 0.0) {
			continue;
		}

		// Divide all elements in this row by the first element since it should be 1
		// we already assured that the pivot cannot be zero, so we can divide by it
		auto factor = mat[r][c];
		for(auto i = c; i < C; ++i) {
			mat[r][i] /= factor;
		}

		// Now add a multiple of the current row to all other rows, so that this column
		// will be set to 0 everywhere except the current line
		for(auto i = r + 1; i < R; ++i) {
			auto fac = mat[i][c];
			for(auto j = c; j < C; ++j) {
				mat[i][j] -= fac * mat[r][j];
			}
		}

		++r; // go to the next row
	}
}

/// Same as nytl::mat::rowEcholon, but operates on a copy.
/// Gives the new matrix TN (= double) precision type to assure
/// there will be no rounding issues.
template<size_t R, size_t C, typename T, typename TN = double>
constexpr auto rowEcholonCopy(const nytl::Mat<R, C, T>& mat) {
	auto ret = static_cast<nytl::Mat<R, C, TN>>(mat);
	rowEcholon(ret);
	return ret;
}

/// Brings the given matrix into the reduced row echolon form (rref).
/// Implements the full Gaussian elimination for a given matrix.
/// The given matrix can be in any form.
/// Complexity Lies within O(n^3) where n is the number of rows/cols of the given matrix.
/// NOTE: This operation divides by values from the matrix so it must have a type does
/// correctly implement division over the desired field (e.g. integer matrices will result
/// in errors here).
template<size_t R, size_t C, typename T>
constexpr void reducedRowEcholon(nytl::Mat<R, C, T>& mat) {
	// first bring the matrix into row-echolon form
	rowEcholon(mat);

	// start with the bottom and eliminate all coefficients above this row that
	// are in the same c
	for(auto r = R; r-- > 0; ) {

		//find the pivot
		auto c = 0u;
		while(c < mat.cols() && mat[r][c] == T{0}) {
			++c;
		}

		// if the pivot is zero continue to the next (above) row
		if(!mat[r][c]) {
			continue;
		}

		// eliminate other coefficients in the current column above the current row
		for(auto p = 0u; p < r; ++p) {
			auto fac = mat[p][c] / mat[r][c];
			for(auto q = 0u; q < C; ++q) {
				mat[p][q] -= fac * mat[r][q];
			}
		}
	}
}

/// Same as nytl::mat::reducedRowEcholon, but operates on a copy.
/// Gives the new matrix TN (= double) precision type to assure
/// there will be no rounding issues.
template<size_t R, size_t C, typename T, typename TN = double>
constexpr auto reducedRowEcholonCopy(const nytl::Mat<R, C, T>& mat) {
	auto ret = static_cast<nytl::Mat<R, C, TN>>(mat);
	reducedRowEcholon(ret);
	return ret;
}

/// Computes a LU decomposition of a given square matrix.
/// Returns a struct with the lower (0) and upper (1) matrix of the decomposition, as
/// well as the used permutation matrix (2) and the sign of the permutation (3).
/// The sign is a value of the field the matrix is defined over. It's 1 if an even number
/// of permutation were performed, or -1 if an uneven number of permutations were performed.
/// The returned lower matrix will have only ones in its diagonal.
/// Works for every square matrix, even singular ones like the zero matrix.
/// The returned matrices always fulfill the equation: PA = LU, where P is the permutation
/// matrix, A the given matrix, L the lower and U the upper matrix.
/// Read more about lu decomposition at https://en.wikipedia.org/wiki/LU_decomposition.
/// The returned matrices have the full field precision type, since this operation divides values.
/// This function cannot fail in any way.
/// Complexity lies within O(n^3) where n is the number of rows/cols of the given matrix.
template<size_t D, typename T, typename R = double>
constexpr LUDecomposition<D, R> luDecomp(const nytl::Mat<D, D, T>& mat) {
	LUDecomposition<D, R> ret {};
	identity(ret.perm);
	ret.upper = static_cast<decltype(ret.upper)>(mat);

	for(auto n = 0u; n < D; ++n) {

		// since we divide by upper[n][n] later on we should try to make it non-zero by
		// swapping the current row with another row. If we do so, we have to pretend we
		// swapped the matrix in the beginning and therefore also change the lower matrix and
		// remember the swap in the permutation matrix
		if(ret.upper[n][n] == 0.0) {
			for(auto r = n + 1; r < D; ++r) {
				if(ret.upper[r][n] != 0.0) {
					swapRow(ret.perm, r, n);
					swapRow(ret.upper, r, n);
					swapRow(ret.lower, r, n);
					ret.sign *= -1;
					break;
				}
			}

			// If all coefficients in the column are zero (e.g. a zero matrix), its ok since
			// we don't have any more coefficients to eliminate.
			if(ret.upper[n][n] == 0.0) {
				ret.lower[n][n] = 1.0;
				continue;
			}
		}

		ret.lower[n][n] = 1.0;

		// erase all coefficients in the nth column below the nth row.
		// pivoting already assured that mat[n][n] is not zero
		for(auto i = n + 1; i < D; ++i) {
			auto fac = ret.upper[i][n] / ret.upper[n][n];
			ret.upper[i] = ret.upper[i] - fac * ret.upper[n];
			ret.lower[i][n] = fac;
		}
	}

	return ret;
}

/// Returns the vector x so that LUx = b.
/// Can be used to more efficiently solve multiple linear equation systems for the
/// same matrix by first decomposing it and then use this function instead of the default
/// Gaussian elimination implementation.
/// The given matrix must be a square matrix.
/// - If the lu composition was done with a permutation matrix (PA = LU), the given
///   vector must be premultiplied with the permutations inverse (tranpose since symmetrical) to
///   get the vector that solves Ax = b. If PA = LU and Ax = b, so LUx = P * b
/// - If the lower or upper matrix are singular, the equation does not have
///   a unique solution and this function results in undefined behvaiour. So
///   if the lower or upper matrix might be singular, check first manually.
/// - The returned vector has a R precision type (defaulted to double), since
///   this operation divides values.
/// - Complexity Lies within O(n^2) where n is the number of rows/cols of the given matrices.
template<size_t D, typename T1, typename T2, typename R = double>
constexpr auto luEvaluate(const Mat<D, D, T1>& l,
		const Mat<D, D, T1>& u,
		const Vec<D, T2>& b) {

	// forward substitution
	Vec<D, R> d {};
	for(auto i = 0u; i < D; ++i) {
		d[i] = b[i];
		for(auto j = 0u; j < i; ++j)
			d[i] -= l[i][j] * d[j];

		d[i] /= l[i][i];
	}

	// back substitution
	Vec<D, R> x {};
	for(auto i = D; i-- > 0; ) {
		x[i] = d[i];
		for(auto j = i + 1; j < D; ++j)
			x[i] -= u[i][j] * x[j];

		x[i] /= u[i][i];
	}

	return x;
}

/// Overload of luEvaluate that returns the vector x so that
/// the decomposed matrix A times x results in b.
/// See the first luEvaluate overload for more details, especially the
/// undefined behavior when A (and therefore the decomposition) is singular.
/// The sign of the decomposition will not be used.
template<size_t D, typename T1, typename T2>
constexpr auto luEvaluate(const LUDecomposition<D, T1>& lu, const Vec<D, T2>& b) {
	return luEvaluate(lu.lower, lu.upper, transpose(lu.perm) * b);
}

/// Returns the determinant of the given square matrix.
/// Complexity Lies within O(n^3) where n is the number of rows/cols of the given matrix.
/// If you already have a lu-decomposition, see the next overload of this function.
template<size_t D, typename T>
constexpr auto determinant(const Mat<D, D, T>& mat) {
	auto [l, u, p, s] = luDecomp(mat);
	unused(l, p);
	return s * static_cast<T>(multiplyDiagonal(u));
}

/// Returns the determinant for the lu decomposition of a matrix.
/// The permutation matrix of the decomposition will not be used.
template<size_t D, typename T>
constexpr auto determinant(const LUDecomposition<D, T>& lu) {
	return lu.sign * multiplyDiagonal(lu.upper) * multiplyDiagonal(lu.lower);
}

/// Returns whether the given square matrix can be inverted.
/// Complexity Lies within O(n^3) where n is the number of rows/cols of the given matrix.
/// If you already know the lu decomposition of this matrix it is much more efficient
/// to calculate the determinant using the decomposition.
template<size_t D, typename T>
constexpr bool invertible(const SquareMat<D, T>& mat) {
	return (determinant(mat) != 0);
}

/// Returns the inverse of the given matrix.
/// If you already know its lu decomposition, see the overload below.
/// Undefined behvaiour if the given function is not invertible.
/// For small matrices (i.e. D <= 4) there are more optimized versions.
template<size_t D, typename T>
constexpr auto inverse(const Mat<D, D, T>& mat) {
	auto lu = luDecomp(mat);
	return inverse(lu);
}

/// Returns the inverse of the matrix that is represented by the
/// given lu decomposition.
/// Undefined behvaiour if the given function is not invertible, check
/// this by simply calculating its determinant from the lower and upper matrices.
template<size_t D, typename T>
constexpr auto inverse(const LUDecomposition<D, T>& lu) {
	Mat<D, D, T> ret {};
	for(auto i = 0u; i < D; ++i) {
		col(ret, i, luEvaluate(lu.lower, lu.upper, col(lu.perm, i)));
	}

	return ret;
}

/// Returns whether the given quadratic matrix is symmetric.
template<size_t D, typename T>
constexpr bool symmetric(const nytl::Mat<D, D, T>& mat) {
	for(auto r = 0u; r < D; ++r) {
		for(auto c = r + 1; c < D; ++c) {
			if(mat[r][c] != mat[c][r]) {
				return false;
			}
		}
	}

	return true;
}

} // namespace nytl

#endif // header guard
