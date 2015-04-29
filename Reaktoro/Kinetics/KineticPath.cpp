// Reaktoro is a C++ library for computational reaction modelling.
//
// Copyright (C) 2014 Allan Leal
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include "KineticPath.hpp"

// C++ includes
#include <functional>
using namespace std::placeholders;

// Reaktoro includes
#include <Reaktoro/Common/ChemicalVector.hpp>
#include <Reaktoro/Common/Exception.hpp>
#include <Reaktoro/Common/Matrix.hpp>
#include <Reaktoro/Common/StringUtils.hpp>
#include <Reaktoro/Common/Units.hpp>
#include <Reaktoro/Core/ChemicalState.hpp>
#include <Reaktoro/Core/ChemicalSystem.hpp>
#include <Reaktoro/Core/Partition.hpp>
#include <Reaktoro/Core/ReactionSystem.hpp>
#include <Reaktoro/Equilibrium/EquilibriumProblem.hpp>
#include <Reaktoro/Equilibrium/EquilibriumResult.hpp>
#include <Reaktoro/Equilibrium/EquilibriumPath.hpp>
#include <Reaktoro/Kinetics/KineticOptions.hpp>
#include <Reaktoro/Kinetics/KineticProblem.hpp>
#include <Reaktoro/Thermodynamics/Water/WaterConstants.hpp>

namespace Reaktoro {

struct KineticPath::Impl
{
    /// The kinetically-controlled chemical reactions
    ReactionSystem reactions;

    /// The chemical system instance
    ChemicalSystem system;

    /// The partition of the species in the chemical system
    Partition partition;

    /// The options of the kinetic Path
    KineticOptions options;

    /// The equilibrium Path instance
    EquilibriumPath equilibrium;

    /// The ODE Path instance
    ODEPath ode;

    /// The indices of the equilibrium and kinetic species
    Indices ispecies_e, ispecies_k;

    /// The indices of the elements in the equilibrium and kinetic partition
    Indices ielements_e, ielements_k;

    /// The number of equilibrium and kinetic species
    unsigned Ne, Nk;

    /// The number of elements in the equilibrium and kinetic partition
    unsigned Ee, Ek;

    /// The formula matrix of the equilibrium species
    Matrix We;

    /// The stoichiometric matrix w.r.t. the equilibrium species
    Matrix Se;

    /// The stoichiometric matrix w.r.t. the kinetic species
    Matrix Sk;

    /// The coefficient matrix `A` of the chemical kinetics problem
    Matrix A;

    /// The temperature of the chemical system (in units of K)
    double T;

    /// The pressure of the chemical system (in units of Pa)
    double P;

    /// The molar composition of the equilibrium species
    Vector ne;

    /// The molar composition of the kinetic species
    Vector nk;

    /// The molar abundance of the elements in the equilibrium species
    Vector be;

    /// The combined vector of elemental molar abundance and composition of kinetic species [be nk]
    Vector benk;

    /// The activities of all species
    ChemicalVector a;

    /// The kinetic rates of the reactions
    ChemicalVector r;

    /// The partial derivatives of the amounts of the equilibrium species w.r.t. amounts of equilibrium elements
    Matrix Be;

    /// The Jacobian of the kinetic rate w.r.t. the equilibrium species
    Matrix Re;

    /// The Jacobian of the kinetic rate w.r.t. the kinetic species
    Matrix Rk;

    // The partial derivatives of the reaction rates `r` w.r.t. to `u = [be nk]`
    Matrix R;

    Impl(const ReactionSystem& reactions)
    : reactions(reactions), system(reactions.system()), equilibrium(system)
    {
        setPartition(Partition(system));
    }

    auto setOptions(const KineticOptions& options_) -> void
    {
        // Initialise the options of the kinetic Path
        options = options_;

        // Initialise the options of other Paths
        ode.setOptions(options.ode);
        equilibrium.setOptions(options.equilibrium);
    }

    auto setPartition(const Partition& partition_) -> void
    {
        // Initialise the partition member
        partition = partition_;

        // Set the partition of the equilibrium Path
        equilibrium.setPartition(partition);

        // Set the indices of the equilibrium and kinetic species
        ispecies_e = partition.indicesEquilibriumSpecies();
        ispecies_k = partition.indicesKineticSpecies();

        // Set the indices of the equilibrium and kinetic elements
        ielements_e = partition.indicesEquilibriumElements();
        ielements_k = partition.indicesKineticElements();

        // Set the number of equilibrium and kinetic species
        Ne = ispecies_e.size();
        Nk = ispecies_k.size();

        // Set the number of equilibrium and kinetic elements
        Ee = ielements_e.size();
        Ek = ielements_k.size();

        // Initialise the formula matrix of the equilibrium partition
        We = partition.formulaMatrixEquilibriumSpecies();

        // Initialise the stoichiometric matrices w.r.t. the equilibrium and kinetic species
        Se = cols(reactions.stoichiometricMatrix(), ispecies_e);
        Sk = cols(reactions.stoichiometricMatrix(), ispecies_k);

        // Initialise the coefficient matrix `A` of the chemical kinetics problem
        A.resize(Ee + Nk, reactions.numReactions());
        A.topRows(Ee) = We * tr(Se);
        A.bottomRows(Nk) = tr(Sk);

        // Allocate memory for the partial derivatives of the reaction rates `r` w.r.t. to `u = [be nk]`
        R.resize(reactions.numReactions(), Ee + Nk);
    }

    auto setPartition(std::string partition) -> void
    {
        setPartition(Partition(system, partition));
    }

    auto initialize(ChemicalState& state, double tstart) -> void
    {
        // Initialise the temperature and pressure variables
        T = state.temperature();
        P = state.pressure();

        // Extract the composition of the equilibrium and kinetic species
        const Vector& n = state.speciesAmounts();
        rows(n, ispecies_e).to(ne);
        rows(n, ispecies_k).to(nk);

        // Assemble the vector benk = [be nk]
        benk.resize(Ee + Nk);
        benk.segment(00, Ee) = We * ne;
        benk.segment(Ee, Nk) = nk;

        // Define the ODE function
        ODEFunction ode_function = [&](double t, const Vector& u, Vector& res)
        {
            return function(state, t, u, res);
        };

        // Define the jacobian of the ODE function
        ODEJacobian ode_jacobian = [&](double t, const Vector& u, Matrix& res)
        {
            return jacobian(state, t, u, res);
        };

        // Initialise the ODE problem
        ODEProblem problem;
        problem.setNumEquations(Ee + Nk);
        problem.setFunction(ode_function);
        problem.setJacobian(ode_jacobian);

        // Set the ODE problem and initialize the ODE Path
        ode.setProblem(problem);
        ode.initialize(tstart, benk);
    }

    auto step(ChemicalState& state, double& t) -> void
    {
        const double tfinal = unsigned(-1);
        step(state, t, tfinal);
    }

    auto step(ChemicalState& state, double& t, double tfinal) -> void
    {
        // Extract the composition vector of the equilibrium and kinetic species
        const Vector& n = state.speciesAmounts();
        rows(n, ispecies_e).to(ne);
        rows(n, ispecies_k).to(nk);

        // Assemble the vector benk = [be nk]
        benk.segment(00, Ee) = We * ne;
        benk.segment(Ee, Nk) = nk;

        // Perform one ODE step integration
        ode.integrate(t, benk, tfinal);

        // Extract the `be` and `nk` entries of the vector `benk`
        be = benk.segment(00, Ee);
        nk = benk.segment(Ee, Nk);

        // Update the composition of the kinetic species
        state.setSpeciesAmounts(nk, ispecies_k);

        // Update the composition of the equilibrium species
        equilibrium.solve(state, be);
    }

    auto solve(ChemicalState& state, double t, double dt) -> void
    {
        if(options.output.active) solveWithOutput(state, t, dt);
        else solveWithoutOutput(state, t, dt);
    }

    auto solveWithoutOutput(ChemicalState& state, double t, double dt) -> void
    {
        // Initialise the chemical kinetics Path
        initialize(state, t);

        // Integrate the chemical kinetics ODE from `t` to `t + dt`
        ode.solve(t, dt, benk);

        // Extract the `be` and `nk` entries of the vector `benk`
        be = benk.segment(00, Ee);
        nk = benk.segment(Ee, Nk);

        // Update the composition of the kinetic species
        state.setSpeciesAmounts(nk, ispecies_k);

        // Update the composition of the equilibrium species
        equilibrium.solve(state, be);
    }

    auto solveWithOutput(ChemicalState& state, double t, double dt) -> void
    {
        // Initialise the chemical kinetics Path
        initialize(state, t);

        // The final time
        const double tfinal = t + dt;

        // Print the header of the output
        outputHeader();

        // Perform one ODE step integration
        while(t < tfinal)
        {
            ode.integrate(t, benk, tfinal);
            outputState(state, t);
        }

        // Extract the `be` and `nk` entries of the vector `benk`
        be = benk.segment(00, Ee);
        nk = benk.segment(Ee, Nk);

        // Update the composition of the kinetic species
        state.setSpeciesAmounts(nk, ispecies_k);

        // Update the composition of the equilibrium species
        equilibrium.solve(state, be);
    }

    auto outputHeader() -> void
    {
        auto words = split(options.output.format);

        for(auto word : words)
            std::cout << std::setw(20) << std::left << word;

        std::cout << std::endl;
    }

    auto outputState(const ChemicalState& state, double t) -> void
    {
        const auto& T = state.temperature();
        const auto& P = state.pressure();
        const auto& n = state.speciesAmounts();
        const auto& a = system.activities(T, P, n);
        const auto& r = reactions.rates(T, P, n, a);

        auto words = split(options.output.format);

        for(auto word : words)
        {
            auto tmp = split(word, ":");

            std::string quantity = tmp[0];
            std::string units = tmp.size() > 1 ? tmp[1] : "";

            if(quantity == "t")
            {
                units = units.empty() ? "seconds" : units;
                std::cout << std::setw(20) << std::left <<
                    units::convert(t, "seconds", units);
            }
            if(quantity[0] == 'n')
            {
                units = units.empty() ? "mol" : units;
                std::string species = split(quantity, "[]").back();
                const double ni = state.speciesAmount(species, units);
                std::cout << std::setw(20) << std::left << ni;
            }
            if(quantity[0] == 'b')
            {
                units = units.empty() ? "mol" : units;
                auto names = split(quantity, "[]");
                std::string element = names[1];
                std::string phase = names.size() > 2 ? names[2] : "";
                const double bi = phase.empty() ?
                    state.elementAmount(element, units) :
                    state.elementAmountInPhase(element, phase, units);
                std::cout << std::setw(20) << std::left << bi;
            }
            if(quantity[0] == 'm')
            {
                units = units.empty() ? "molal" : units;
                std::string species = split(quantity, "[]").back();
                const double nH2O = state.speciesAmount("H2O(l)");
                const double ni = state.speciesAmount(species);
                const double mi = ni/(nH2O * waterMolarMass);
                std::cout << std::setw(20) << std::left <<
                    units::convert(mi, "molal", units);
            }
            if(quantity[0] == 'r')
            {
                units = units.empty() ? "mol/s" : units;
                std::string reaction = split(quantity, "[]").back();
                Index index = reactions.indexReaction(reaction);
                const double ri = r.val[index];
                std::cout << std::setw(20) << std::left <<
                    units::convert(ri, "mol/s", units);
            }
            if(quantity[0] == 'a')
            {
                std::string species = split(quantity, "[]").back();
                Index index = system.indexSpecies(species);
                std::cout << std::setw(20) << std::left << a.val[index];
            }
            if(quantity == "pH")
            {
                const Index iH = system.indexSpecies("H+");
                const double aH = a.val[iH];
                std::cout << std::setw(20) << std::left << -std::log10(aH);
            }
        }

        std::cout << std::endl;
    }

    auto function(ChemicalState& state, double t, const Vector& u, Vector& res) -> int
    {
        // Extract the `be` and `nk` entries of the vector [be, nk]
        be = u.segment(00, Ee);
        nk = u.segment(Ee, Nk);

        // Check for non-finite values in the vector `benk`
        for(unsigned i = 0; i < u.rows(); ++i)
            if(not std::isfinite(u[i]))
                return 1; // ensure the ode Path will reduce the time step

        // Update the composition of the kinetic species in the member `state`
        state.setSpeciesAmounts(nk, ispecies_k);

        // Solve the equilibrium problem using the elemental molar abundance `be`
        equilibrium.solve(state, be);

        // Get the molar amounts of the species
        const Vector& n = state.speciesAmounts();

        // Update the activities of the species
        a = system.activities(T, P, n);

        // Calculate the kinetic rates of the reactions
        r = reactions.rates(T, P, n, a);

        // Calculate the right-hand side function of the ODE
        res.segment(00, Ee) = We * tr(Se) * r.val;
        res.segment(Ee, Nk) = tr(Sk) * r.val;

        // Impose a lower bound for the decrease of some kinetic species
        for(unsigned i = 0; i < u.rows(); ++i)
            if(std::abs(u[i]) < 1.0e-50 and res[i] < 0.0)
                res[i] = 0.0; // set the rate to zero

        return 0;
    }

    auto jacobian(ChemicalState& state, double t, const Vector& u, Matrix& res) -> int
    {
        // Extract the `be` and `nk` entries of the vector `benk = [be, nk]`
        be = u.segment(00, Ee);
        nk = u.segment(Ee, Nk);

        // Update the composition of the kinetic species in the member `state`
        state.setSpeciesAmounts(nk, ispecies_k);

        // Solve the equilibrium problem using the elemental molar abundance `be`
        equilibrium.solve(state, be);

        // Calculate the partial derivatives of the amounts of the equilibrium species w.r.t. amounts of equilibrium elements
        Be = equilibrium.dndb(state);

        // Extract the columns of the jacobian matrix w.r.t. the equilibrium and kinetic species
        Re = cols(r.ddn, ispecies_e);
        Rk = cols(r.ddn, ispecies_k);

        // Calculate the partial derivatives of the reaction rates `r` w.r.t. to `u = [be nk]`
        R.leftCols(Ee) = Re * Be;
        R.rightCols(Nk) = Rk;

        res = A * R;

        return 0;
    }
};

KineticPath::KineticPath(const ReactionSystem& reactions)
: pimpl(new Impl(reactions))
{}

KineticPath::KineticPath(const KineticPath& other)
: pimpl(new Impl(*other.pimpl))
{}

KineticPath::~KineticPath()
{}

auto KineticPath::operator=(KineticPath other) -> KineticPath&
{
    pimpl = std::move(other.pimpl);
    return *this;
}

auto KineticPath::setOptions(const KineticOptions& options) -> void
{
    pimpl->setOptions(options);
}

auto KineticPath::setPartition(const Partition& partition) -> void
{
    pimpl->setPartition(partition);
}

auto KineticPath::setPartition(std::string partition) -> void
{
    pimpl->setPartition(partition);
}

auto KineticPath::initialize(ChemicalState& state, double tstart) -> void
{
    pimpl->initialize(state, tstart);
}

auto KineticPath::step(ChemicalState& state, double& t) -> void
{
    pimpl->step(state, t);
}

auto KineticPath::step(ChemicalState& state, double& t, double dt) -> void
{
    pimpl->step(state, t, dt);
}

auto KineticPath::solve(ChemicalState& state, double t, double dt) -> void
{
    pimpl->solve(state, t, dt);
}

} // namespace Reaktoro