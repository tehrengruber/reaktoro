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

#include "AqueousPhase.hpp"

// Reaktor includes
#include <Reaktor/Activity/AqueousActivityDrummond.hpp>
#include <Reaktor/Activity/AqueousActivityDuanSun.hpp>
#include <Reaktor/Activity/AqueousActivityHKF.hpp>
#include <Reaktor/Activity/AqueousActivityIdeal.hpp>
#include <Reaktor/Activity/AqueousActivityPitzer.hpp>
#include <Reaktor/Activity/AqueousActivityRumpf.hpp>
#include <Reaktor/Activity/AqueousActivitySetschenow.hpp>
#include <Reaktor/Common/Index.hpp>
#include <Reaktor/Common/Vector.hpp>
#include <Reaktor/Thermo/WaterConstants.hpp>

namespace Reaktor {

AqueousPhase::AqueousPhase()
: AqueousMixture()
{}

AqueousPhase::AqueousPhase(const std::vector<AqueousSpecies>& species)
: AqueousMixture(species), activities$(species.size())
{
    for(const auto& iter : species)
        setActivityModelSetschenow(iter.name(), 0.1);
}

auto AqueousPhase::setActivityModel(const std::string& species, const AqueousActivity& activity) -> void
{
    const Index ispecies = idxSpecies(species);
    if(ispecies < numSpecies())
        activities$[ispecies] = activity;
}

auto AqueousPhase::setActivityModelIdeal(const std::string& species) -> void
{
    const Index ispecies = idxSpecies(species);
    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityIdeal(species, *this);
}

auto AqueousPhase::setActivityModelSetschenow(const std::string& species, double b) -> void
{
    const Index ispecies = idxSpecies(species);

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivitySetschenow(species, *this, b);
}

auto AqueousPhase::setActivityModelDuanSunCO2() -> void
{
    const Index ispecies = idxSpecies("CO2(aq)");

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityDuanSunCO2(*this);
}

auto AqueousPhase::setActivityModelDrummondCO2() -> void
{
    const Index ispecies = idxSpecies("CO2(aq)");

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityDrummondCO2(*this);
}

auto AqueousPhase::setActivityModelRumpfCO2() -> void
{
    const Index ispecies = idxSpecies("CO2(aq)");

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityRumpfCO2(*this);
}

auto AqueousPhase::setActivityModelHKFWater() -> void
{
    const Index ispecies = idxSpecies("H2O(l)");

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityHKFWater(*this);
}

auto AqueousPhase::setActivityModelHKFChargedSpecies() -> void
{
    for(Index idx : idxChargedSpecies())
        activities$[idx] = aqueousActivityHKFCharged(species(idx).name(), *this);
}

auto AqueousPhase::setActivityModelPitzerWater() -> void
{
    const Index ispecies = idxSpecies("H2O(l)");

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityPitzerWater(*this);
}

auto AqueousPhase::setActivityModelPitzerChargedSpecies() -> void
{
    for(Index idx : idxChargedSpecies())
        activities$[idx] = aqueousActivityPitzerCharged(species(idx).name(), *this);
}

auto AqueousPhase::setActivityModelPitzerNeutralSpecies(const std::string& species) -> void
{
    const Index ispecies = idxSpecies(species);

    if(ispecies < numSpecies())
        activities$[ispecies] = aqueousActivityPitzerNeutral(species, *this);
}

auto AqueousPhase::params(double T, double P, const Vector& n) const -> AqueousActivityParams
{
    AqueousActivityParams params;

    params.T  = T;
    params.P  = P;
    params.n  = n;
    params.x  = molarFractions(n);
    params.m  = molalities(n);
    params.ms = stoichiometricMolalities(params.m);
    params.Ie = effectiveIonicStrength(params.m);
    params.Is = stoichiometricIonicStrength(params.ms);

    return params;
}

auto AqueousPhase::concentrations(const Vector& n) const -> Vector
{
    // The total amount of moles in the aqueous phase
    const double ntotal = n.sum();

    // Check if the phase has zero number of moles
    if(ntotal == 0.0) return zeros(n.rows());

    // The index of the water species
    const Index iH2O = indexWater();

    // Calculate the mass of H2O in the phase (in units of kg)
    const double massH2O = n[iH2O] * waterMolarMass;

    // Calculate the molalities of the aqueous species
    Vector c = n/massH2O;

    // Set the concentration of water to its molar fraction
    c[iH2O] = n[iH2O]/ntotal;

    return c;
}

auto AqueousPhase::activities(double T, double P, const Vector& n) const -> VectorResult
{
    AqueousActivityParams pars = params(T, P, n);

    const unsigned N = numSpecies();

    VectorResult a = partialVector(zeros(N), zeros(N, N));

    for(unsigned i = 0; i < N; ++i)
    {
        const ScalarResult res = activities$[i](pars);

        func(a)[i] = func(res);
        grad(a).row(i) = grad(res);
    }

    return a;
}

} // namespace Reaktor
