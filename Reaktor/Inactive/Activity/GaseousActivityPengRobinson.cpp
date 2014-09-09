/*
 * Reaktor is a C++ library for computational reaction modelling.
 *
 * Copyright (C) 2014 Allan Leal
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "GaseousActivityPengRobinson.hpp"

// C++ includes
#include <functional>
using namespace std::placeholders;

// Reaktor includes
#include <Reaktor/Common/Index.hpp>
#include <Reaktor/External/Units/ConversionUtils.hpp>
#include <Reaktor/Math/Roots.hpp>
#include <Reaktor/Mixtures/GaseousMixture.hpp>
#include <Reaktor/Common/ConvertUtils.hpp>

namespace Reaktor {
namespace internal {

// The critical pressure of selected gases (in units of kelvin)
std::map<std::string, double> criticalT =
{
    {"CO2(g)", 304.25}
};

// The critical pressure of selected gases (in units of bar)
std::map<std::string, double> criticalP =
{
    {"CO2(g)", 73.9}
};

// The acentric factor of selected gases
std::map<std::string, double> acentric_factor =
{
    {"CO2(g)", 0.225}
};

auto calculateKappa(double w) -> double
{
    return (w <= 0.49) ?
        0.374640 + 1.54226*w - 0.269920*w*w :
        0.379642 + 1.48503*w - 0.164423*w*w + 0.016666*w*w*w;
}

struct GasData
{
    GasData();

    explicit GasData(const std::string& gas);

    double Tc;

    double Pc;

    double omega;

    double kappa;
};

GasData::GasData()
{}

GasData::GasData(const std::string& gas)
{
    Tc = criticalT[gas];
    Pc = units::convert(unit(bar), unit(pascal), criticalP[gas]);
    omega = acentric_factor[gas];
    kappa = calculateKappa(omega);
}

auto gaseousActivityPengRobinson(const GaseousActivityParams& params, const GasData& gas_data, const Index& idx_species) -> ScalarResult
{
    const double T  = params.T; // in units of K
    const double P  = params.P; // in units of Pa
    const double Tc = gas_data.Tc; // in units of K
    const double Pc = gas_data.Pc; // in units of Pa
    const double Tr = T/Tc; // dimensionless
    const double R = 8.3144621; // in units of J/(mol*K)
    const double kappa = gas_data.kappa;
    const double ac = 0.45724 * (R*R*Tc*Tc)/Pc;
    const double a = std::pow(1 + kappa*(1 - std::sqrt(Tr)), 2) * ac;
    const double b = 0.07780 * (R*Tc)/Pc;
    const double A = (a*P)/(R*R*T*T);
    const double B = (b*P)/(R*T);
    const double sqrt2 = 1.4142136;

    const double c0 = 1;
    const double c1 = B - 1;
    const double c2 = A - 2*B - 3*B*B;
    const double c3 = B*B*B + B*B - A*B;

    std::complex<double> r1, r2, r3;

    std::tie(r1, r2, r3) = cubicRoots(c0, c1, c2, c3);

    double Z;

    if(r2.imag() and r3.imag())
        Z = r1.real();
    else
    {
        const double Zl = std::min(r1.real(), std::min(r2.real(), r3.real()));
        const double Zg = std::max(r1.real(), std::max(r2.real(), r3.real()));

        const double xl = ((sqrt2 + 1)*B + Zl)/((sqrt2 - 1)*B - Zl);
        const double xg = ((sqrt2 + 1)*B + Zg)/((sqrt2 - 1)*B - Zg);

        const double w1 = Zg - Zl;
        const double w2 = std::log((Zg - B)/(Zl - B)) + A/(2*sqrt2*B) * std::log(xg/xl);

        Z = (w2 < w1) ? Zl : Zg;
    }

    // The natural log of the fugacity coefficient of the gaseous species
    const double log_phi = Z - 1 - std::log(Z - B) - A/(2*sqrt2*B) * std::log((Z + (sqrt2 + 1)*B)/(Z - (sqrt2 - 1)*B));

    // The fugacity coefficient of the gaseous species
    const double phi = std::exp(log_phi);

    // The pressure (in units of bar)
    const double Pb = convert<Pa,bar>(params.P);

    // The molar fraction of the given gaseous species and its molar partial derivatives
    ScalarResult xi = partialScalar(params.x, idx_species);

    // The activity of the gaseous species
    ScalarResult ai;
    func(ai) = func(xi) * phi * Pb;
    grad(ai) = grad(xi) * phi * Pb;

    return ai;
}

} /* namespace internal */

auto gaseousActivityPengRobinson(const std::string& species, const GaseousMixture& mixture) -> GaseousActivity
{
    const Index idx_species = indexSpecies(mixture, species);

    internal::GasData gas_data(species);

    return std::bind(internal::gaseousActivityPengRobinson, _1, gas_data, idx_species);
}

} // namespace Reaktor
