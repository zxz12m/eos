/* vim: set sw=4 sts=4 et foldmethod=syntax : */

/*
 * Copyright (c) 2014, 2015, 2020 Danny van Dyk
 * Copyright (c) 2019, 2020 Domagoj Leljak
 * Copyright (c) 2022 Aliaksei Kachanovich
 *
 * This file is part of the EOS project. EOS is free software;
 * you can redistribute it and/or modify it under the terms of the GNU General
 * Public License version 2, as published by the Free Software Foundation.
 *
 * EOS is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <eos/form-factors/analytic-d-to-pi.hh>
#include <eos/form-factors/pi-lcdas.hh>
#include <eos/maths/derivative.hh>
#include <eos/utils/exception.hh>
#include <eos/maths/integrate.hh>
#include <eos/utils/log.hh>
#include <eos/models/model.hh>
#include <eos/utils/options-impl.hh>
#include <eos/maths/polylog.hh>
#include <eos/maths/power-of.hh>
#include <eos/utils/private_implementation_pattern-impl.hh>
#include <eos/utils/qcd.hh>

#include <functional>
#include <limits>

#include <gsl/gsl_sf_gamma.h>

namespace eos
{
    template <>
    struct Implementation<AnalyticFormFactorDToPiKKMO2009>
    {
        std::shared_ptr<Model> model;

        // hadronic parameters
        UsedParameter MD;
        UsedParameter mpi;
        UsedParameter fpi;

        // Borel parameters, thresholds and renormalization scale
        SwitchOption opt_rescale_borel;
        std::function<double (const double &)> rescale_factor_p;
        std::function<double (const double &)> rescale_factor_0;
        std::function<double (const double &)> rescale_factor_T;
        UsedParameter M2;
        UsedParameter Mprime2;
        UsedParameter _s0_plus, _s0_plus_p, _s0_plus_pp;
        UsedParameter _s0_zero, _s0_zero_p, _s0_zero_pp;
        UsedParameter _s0_T,    _s0_T_p,    _s0_T_pp;
        UsedParameter sprime0B;
        UsedParameter mu;

        // Parameter for the estimation of NNLO corrections
        UsedParameter zeta_nnlo;

        // QCD parameters
        UsedParameter m02;
        UsedParameter cond_GG;
        UsedParameter r_vac;

        PionLCDAs pi;

        GSL::QAGS::Config config;

        static const std::vector<OptionSpecification> options;

        Implementation(const Parameters & p, const Options & o, ParameterUser & u) :
            model(Model::make("SM", p, o)),
            MD(p["mass::D_d"], u),
            mpi(p["mass::pi^+"], u),
            fpi(p["decay-constant::pi"], u),
            opt_rescale_borel(o, "rescale-borel", { "1", "0" }, "1"),
            M2(p["D->pi::M^2@KKMO2009"], u),
            Mprime2(p["D->pi::Mp^2@KKMO2009"], u),
            _s0_plus(p["D->pi::s_0^+(0)@KKMO2009"], u),
            _s0_plus_p(p["D->pi::s_0^+'(0)@KKMO2009"], u),
            _s0_plus_pp(p["D->pi::s_0^+''(0)@KKMO2009"], u),
            _s0_zero(p["D->pi::s_0^0(0)@KKMO2009"], u),
            _s0_zero_p(p["D->pi::s_0^0'(0)@KKMO2009"], u),
            _s0_zero_pp(p["D->pi::s_0^0''(0)@KKMO2009"], u),
            _s0_T(p["D->pi::s_0^T(0)@KKMO2009"], u),
            _s0_T_p(p["D->pi::s_0^T'(0)@KKMO2009"], u),
            _s0_T_pp(p["D->pi::s_0^T''(0)@KKMO2009"], u),
            sprime0B(p["D->pi::sp_0^B@KKMO2009"], u),
            mu(p["D->pi::mu@KKMO2009"], u),
            zeta_nnlo(p["D->pi::zeta(NNLO)@KKMO2009"], u),
            m02(p["QCD::m_0^2"], u),
            cond_GG(p["QCD::cond_GG"], u),
            r_vac(p["QCD::r_vac"], u),
            pi(p, o),
            config(GSL::QAGS::Config().epsrel(1e-3))
        {
            using namespace std::placeholders;

            if ('1' == opt_rescale_borel.value()[0])
            {
                rescale_factor_p = std::bind(&Implementation::_rescale_factor_p, this, _1);
                rescale_factor_0 = std::bind(&Implementation::_rescale_factor_0, this, _1);
                rescale_factor_T = std::bind(&Implementation::_rescale_factor_T, this, _1);

            }
            else
            {
                rescale_factor_p = std::bind(&Implementation::_no_rescale_factor, this, _1);
                rescale_factor_0 = std::bind(&Implementation::_no_rescale_factor, this, _1);
                rescale_factor_T = std::bind(&Implementation::_no_rescale_factor, this, _1);

            }

            u.uses(*model);
        }

        inline double m_c_msbar(const double & mu) const 
        {
            return model->m_c_msbar(mu);  
        }

        inline double s0D(const double & q2) const
        {
            return _s0_plus() + _s0_plus_p() * q2 + _s0_plus_pp() * 0.5 * q2 * q2;
        }

        inline double s0tilD(const double & q2) const 
        {
            return _s0_zero() + _s0_zero_p() * q2 + _s0_zero_pp() * 0.5 * q2 * q2;
        }

        inline double s0TD(const double & q2) const
        {
            return _s0_T() + _s0_T_p() * q2 + _s0_T_pp() * 0.5 * q2 * q2;
        }

        static double rho_1(const double & s, const double & mc, const double & mu)
        {
            const double mc2 = mc * mc, x = mc2 / s;
            const double lnx = std::log(x), ln1mx = std::log(1.0 - x), re_li2_x = real(dilog(complex<double>(x, 0.0)));
            const double lnmumc = std::log(mu / mc);

            double result = s / 2 * (1.0 - x) * (
                    (1.0 - x) * (4.0 * re_li2_x + 2.0 * lnx * ln1mx - (5.0 - 2.0 * x) * ln1mx)
                  + (1.0 - 2.0 * x) * (3.0 - x) * lnx + 3.0 * (1.0 - 3.0 * x) * 2.0 * lnmumc
                  + (17.0 - 33.0 * x) / 2.0
                );

            return result;
        }

        static double delta_1(const double & mc, const double & mu, const double & Mprime2)
        {
            const double mc2 = mc * mc, mu2 = mu * mu;
            const double gamma = gsl_sf_gamma_inc(0.0, mc2 / Mprime2);

            return -3.0 / 2.0 * (gamma * std::exp(mc2 / Mprime2) - 1.0 - (1.0 - mc2 / Mprime2) * (std::log(mu2 / mc2) + 4.0 / 3.0));
        }

        double decay_constant() const
        {
            static const double pi = M_PI, pi2 = pi * pi;
            static const double eps = 1.0e-10;

            const double MD2 = MD * MD, MD4 = MD2 * MD2;
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mc4 = mc2 * mc2;
            const double Mprime4 = Mprime2 * Mprime2;

            const double cond_qq_mu = -fpi * fpi * this->pi.mupi(mu) / 2.0; // <qq>@mu
            const double cond_qq_1 = -fpi * fpi * this->pi.mupi(1.0) / 2.0; // <qq>@1GeV

            const double alpha_s_mu = model->alpha_s(mu());
            const double alpha_s_1 = model->alpha_s(1.0); // alpha_s@1GeV

            using namespace std::placeholders;

            std::function<double (const double &)> integrand(
                [&] (const double & s) -> double
                {
                    return std::exp(-s / Mprime2) * ((s - mc2) * (s - mc2) / s + 4.0 * alpha_s_mu / (3.0 * pi) * rho_1(s, mc, mu));
                }
            );
            const double integral = integrate<GSL::QAGS>(integrand, mc2 + eps, sprime0B, config);

            double result = std::exp(MD2 / Mprime2) / MD4 * (3.0 * mc2 / (8.0 * pi2) * integral
                + mc2 * std::exp(-mc2 / Mprime2) * (
                    - mc * cond_qq_mu * (1.0 + 4.0 * alpha_s_mu / (3.0 * pi) * delta_1(mc, mu, Mprime2))
                    - mc * cond_qq_1  * m02 / (2.0 * Mprime2) * (1.0 - mc2 / (2 * Mprime2))
                    + cond_GG / 12.0
                    - 16.0 * pi * alpha_s_1 * cond_qq_1 * cond_qq_1 * r_vac / (27.0 * Mprime2) * (1.0 - mc2 / (4.0 * Mprime2) - mc4 / (12.0 * Mprime4))
                )
            );

            return std::sqrt(result);
        }

        static double delta_1_M2_deriv(const double & mc, const double & mu, const double & Mprime2)
        {
            const double mc2 = mc * mc, mu2 = mu * mu;
            const double gamma = gsl_sf_gamma_inc(0.0, mc2 / Mprime2);

            return -3.0 / 2.0 * (Mprime2 - mc2 * gamma * std::exp(mc2 / Mprime2) - mc2 * (std::log(mu2 / mc2) + 4.0 / 3.0));
        }

        double MD_svz() const
        {
            static const double pi = M_PI, pi2 = pi * pi;
            static const double eps = 1.0e-10;

            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mc4 = mc2 * mc2;
            const double Mprime4 = Mprime2 * Mprime2;

            const double cond_qq_mu = -fpi * fpi * this->pi.mupi(mu) / 2.0; // <qq>@mu
            const double cond_qq_1 = -fpi * fpi * this->pi.mupi(1.0) / 2.0; // <qq>@1GeV

            const double alpha_s_mu = model->alpha_s(mu());
            const double alpha_s_1 = model->alpha_s(1.0); // alpha_s@1GeV

            using namespace std::placeholders;

            std::function<double (const double &)> integrand_numerator(
                [&] (const double & s) -> double
                {
                    return std::exp(-s / Mprime2) * ((s - mc2) * (s - mc2) + 4.0 * s * alpha_s_mu / (3.0 * pi) * rho_1(s, mc, mu));
                }
            );
            const double integral_numerator = integrate<GSL::QAGS>(integrand_numerator, mc2 + eps, sprime0B, config);
            std::function<double (const double &)> integrand_denominator(
                [&] (const double & s) -> double
                { 
                    return std::exp(-s / Mprime2) * ((s - mc2) * (s - mc2) / s + 4.0 * alpha_s_mu / (3.0 * pi) * rho_1(s, mc, mu));
                }
            );
            const double integral_denominator = integrate<GSL::QAGS>(integrand_denominator, mc2 + eps, sprime0B, config);

            double numerator = 3.0 * mc2 / (8.0 * pi2) * integral_numerator
                + mc4 * std::exp(-mc2 / Mprime2) * (
                    - mc * cond_qq_mu * (1.0 + 4.0 * alpha_s_mu / (3.0 * pi) * delta_1(mc, mu, Mprime2))
                    - mc * cond_qq_1  * m02 / (2.0 * Mprime2) * (1.0 - mc2 / (2 * Mprime2))
                    + cond_GG / 12.0
                    - 16.0 * pi * alpha_s_1 * cond_qq_1 * cond_qq_1 / (27.0 * Mprime2) * (1.0 - mc2 / (4.0 * Mprime2) - mc4 / (12.0 * Mprime4)) 
                )
                + mc2 * std::exp(-mc2 / Mprime2) * (
                    - mc * cond_qq_mu * 4.0 * alpha_s_mu / (3.0 * pi) * delta_1(mc, mu, Mprime2)
                    - mc * cond_qq_1  * m02 / (2.0 * Mprime2) * (mc2 - Mprime2) 
                    + 16.0 * pi * alpha_s_1 * cond_qq_1 * cond_qq_1 / (27.0 * 4.0 * Mprime4) * (4.0 * Mprime4 - 2.0 * Mprime2 * mc2 - mc4)
                );
            double denominator = 3.0 * mc2 / (8.0 * pi2) * integral_denominator
                + mc2 * std::exp(-mc2 / Mprime2) * ( 
                    - mc * cond_qq_mu * (1.0 + 4.0 * alpha_s_mu / (3.0 * pi) * delta_1(mc, mu, Mprime2))
                    - mc * cond_qq_1  * m02 / (2.0 * Mprime2) * (1.0 - mc2 / (2 * Mprime2))
                    + cond_GG / 12.0
                    - 16.0 * pi * alpha_s_1 * cond_qq_1 * cond_qq_1 / (27.0 * Mprime2) * (1.0 - mc2 / (4.0 * Mprime2) - mc4 / (12.0 * Mprime4))
                );

            return std::sqrt(numerator / denominator);
        }

        double F_lo_tw2_integrand(const double & u, const double & q2, const double _M2, const double & _select_weight) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi;

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

            return weight * std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) / u * this->pi.phi(u, mu);
        }

        double F_lo_tw2(const double & q2, const double & _M2, const double & _select_weight = 0.0, const double & _select_corr = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double s0 = s0D(q2) * (1.0 - _select_corr) + s0tilD(q2) * _select_corr;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0 - q2));

            std::function<double (const double &)> integrand(std::bind(&Implementation<AnalyticFormFactorDToPiKKMO2009>::F_lo_tw2_integrand, this, std::placeholders::_1, q2, _M2, _select_weight));

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, u0, 1.000, config);
        }

        double F_lo_tw3_integrand(const double & u, const double & q2, const double & _M2, const double & _select_weight) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi;
            const double mupi = pi.mupi(mu);
            const double omega3pi = pi.omega3pi(mu);

            // auxilliary functions and their first derivatives
            auto I3 = [&] (const double & u) -> double
            {
                const double u3 = u * u * u, ubar2 = (1.0 - u) * (1.0 - u);

                return 5.0 / 2.0 * u3 * ubar2 * (12.0 + (7.0 * u - 4) * omega3pi);
            };
            auto I3_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, ubar = 1.0 - u;

                return 15.0 * u2 * ubar * (6.0 - 10.0 * u - (2.0 - 8.0 * u + 7.0 * u2) * omega3pi);
            };
            auto I3bar = [&] (const double & u) -> double
            {
                const double u3 = u * u * u, ubar2 = (1.0 - u) * (1.0 - u);

                return 5.0 / 2.0 * u3 * ubar2 * (24.0 * u + 6.0 * u * omega3pi - 3.0 * (omega3pi + 4.0));
            };
            auto I3bar_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u;

                return 15.0 / 2.0 * u2 * (12.0 * u3 - 25.0 * u2 + 16.0 * u - 3.0) * (omega3pi + 4.0);
            };

            const double u2 = u * u;
            const double tw3a = pi.phi3p(u, mu)
                + (
                    pi.phi3s(u, mu) / u
                    - (mc2 + q2 - u2 * mpi2) / (2 * (mc2 - q2 + u2 * mpi2)) * pi.phi3s_d1(u, mu)
                    - (2 * u * mpi2 * mc2) / power_of<2>(mc2 - q2 + u2 * mpi2) * pi.phi3s(u, mu)
                ) / 3.0;
            const double tw3b = 2.0 / u * (mc2 - q2 - u2 * mpi2) / (mc2 - q2 + u2 * mpi2)
                * (I3_d1(u) - (2.0 * u * mpi2) / (mc2 - q2 + u2 * mpi2) * I3(u));
            const double tw3c = 3.0 * mpi2 / (mc2 - q2 + u2 * mpi2)
                * (I3bar_d1(u) - (2.0 * u * mpi2) / (mc2 - q2 + u2 * mpi2) * I3bar(u));

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

            return std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2))
                * weight * (mupi / mc * tw3a - pi.f3pi(mu) / (mc * fpi) * (tw3b + tw3c));
        }

        double F_lo_tw3(const double & q2, const double & _M2, const double & _select_weight = 0.0, const double & _select_corr = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double s0 = s0D(q2) * (1.0 - _select_corr) + s0tilD(q2) * _select_corr;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0 - q2));
            std::function<double (const double &)> integrand(std::bind(&Implementation<AnalyticFormFactorDToPiKKMO2009>::F_lo_tw3_integrand, this, std::placeholders::_1, q2, _M2, _select_weight));

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, u0, 1.000, config);
        }

        double F_lo_tw4(const double & q2, const double & _M2, const double & _select_weight = 0.0, const double & _select_corr = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi, mpi4 = mpi2 * mpi2;
            const double s0 = s0D(q2) * (1.0 - _select_corr) + s0tilD(q2) * _select_corr;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0 - q2));
            const double a2pi = pi.a2pi(mu);
            const double deltapipi = pi.deltapipi(mu);
            const double omega4pi = pi.omega4pi(mu);

            // auxilliary functions and their first derivatives
            auto I4 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u;
                const double ubar = 1.0 - u;

                return -1.0 / 24.0 * u * ubar * (
                        mpi2 * (54.0 * u3 - 81.0 * u2 + 27.0 * ubar + 27.0 * a2pi * (16.0 * u3 - 29.0 * u2 + 13.0 * u - 1.0))
                        + 16.0 * u * (20.0 * u - 30.0) * deltapipi
                    );
            };
            auto I4_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u, u4 = u2 * u2;

                return 1.0 / 24 * (
                        27.0 * mpi2 * (
                            (10.0 * u4 - 20.0 * u3 + 6.0 * u2 + 4.0 * u - 1.0)
                            + a2pi * (80.0 * u4 - 180.0 * u3 + 126.0 * u2 - 28.0 * u + 1)
                        )
                        + 160.0 * u * (6.0 - 15.0 * u + 8.0 * u2) * deltapipi
                    );
            };
            auto I4bar = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u;
                const double ubar = 1.0 - u;

                return 1.0 / 48.0 * u * ubar * (
                        mpi2 * (
                            -(54.0 * u3 - 81.0 * u2 - 27.0 * u + 27.0)
                            + 27.0 * a2pi * (32.0 * u3 - 43.0 * u2 + 11.0 * u + 1.0)
                        )
                        - 20.0 * u * (
                            (12.0 - 20.0 * u)
                            + (378.0 * u2 - 567.0 * u + 189.0) * omega4pi
                        ) * deltapipi
                    );
            };
            auto I4barI = [&] (const double & u) -> double
            {
                const double u2 = u * u;
                const double ubar = 1.0 - u, ubar2 = ubar * ubar;

                return 1.0 / 96.0 * u2 * ubar2 * (
                        mpi2 * (
                            9.0 * (3.0 + 2.0 * ubar * u)
                            + 9.0 * a2pi * (32.0 * u2 - 26.0 * u - 3.0)
                        )
                        + 40.0 * u * (4.0 + 63.0 * ubar * omega4pi) * deltapipi
                    );
            };
            auto I4bar_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u, u4 = u2 * u2;

                return 1.0 / 48.0 * (
                        27.0 * mpi2 * (
                            (10.0 * u4 - 20.0 * u3 + 6.0 * u2 + 4.0 * u - 1.0)
                            - a2pi * (160.0 * u4 - 300.0 * u3 + 162.0 * u2 - 20.0 * u - 1.0)
                        )
                        + 40.0 * u * (
                            (-40.0 * u2 + 48.0 * u - 12.0)
                            + 189.0 * (5.0 * u3 - 10.0 *  u2 + 6.0 * u - 1.0) * omega4pi
                        ) * deltapipi
                    );
            };
            std::function<double (const double &)> integrand(
                [&] (const double & u) -> double
                {
                    const double u2 = u * u;

                    const double tw4psi = u * pi.psi4(u, mu) + (mc2 - q2 - u2 * mpi2) / (mc2 - q2 + u2 * mpi2) * pi.psi4_i(u, mu);
                    const double tw4phi = (
                            pi.phi4_d2(u, mu)
                            - 6.0 * u * mpi2 / (mc2 - q2 + u2 * mpi2) * pi.phi4_d1(u, mu)
                            + 12.0 * u * mpi4 / power_of<2>(mc2 - q2 + u2 * mpi2) * pi.phi4(u, mu)
                        ) * mc2 * u / (4 * (mc2 - q2 + u2 * mpi2));
                    const double tw4I4 = I4_d1(u) - 2.0 * u * mpi2 / (mc2 - q2 + u2 * mpi2) * I4(u);
                    const double tw4I4bar1 = (u * I4bar_d1(u) + (mc2 - q2 - 3.0 * u2 * mpi2) / (mc2 - q2 + u2 * mpi2) * I4bar(u)) * 2.0 * u * mpi2 / (mc2 - q2 + u2 * mpi2);
                    const double tw4I4bar2 = (I4bar(u) + 6.0 * u * mpi2 / (mc2 - q2 + u2 * mpi2) * I4barI(u)) * 2.0 * u * mpi2 * (mc2 - q2 - u2 * mpi2) / (mc2 - q2 + u2 * mpi2);

                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

                    return std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) * weight
                        * (tw4psi - tw4phi - tw4I4 - tw4I4bar1 - tw4I4bar2) / (mc2 - q2 + u2 * mpi2);
                }
            );

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, u0, 1 - 1e-10, config);
        }

        double F_nlo_tw2(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double a2pi = pi.a2pi(mu), a4pi = pi.a4pi(mu);
            const double r1 = q2 / mc2;

            // imaginary parts of the hard scattering kernel, integrated over rho.
            auto T1tw2theta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2;
                const double L = std::log(power_of<2>(r2 - 1.0) * mc2 / (mu * mu * r2));

                const double ca0 = power_of<4>(r1 - r2) * (-3.0 + r1 + r2 * 2.0);
                const double ca2 = power_of<2>(r1 - r2) * (
                               (-125.0 + r1  * 155.0  - r12 * 43.0 + r13)
                        + r2 * (220.0 - r1 * 224.0 + r12 * 40.0)
                        + r22 * (-108.0 + 72.0 * r1)
                        + r23 * 12.0);
                const double ca4 = (-3087.0 + r1 * 6804.0 - r12 * 5096.0 + r13 * 1484.0 - r14 * 136.0 + r15)
                        + r2  * (8631.0 - 17024.0 * r1 + 10836.0 * r12 - 2424.0 * r13 + 131.0 * r14)
                        + r22 * (-8750.0 + 14700.0 * r1 - 7200.0 * r12 + 950.0 * r13)
                        + r23 * (3850.0 - r1 * 5000.0 + r12 * 1450.0)
                        + r24 * (-675.0 + r1 * 525.0 )
                        + r25 * 30.0;

                const double cb0 = power_of<4>(r1 - r2);
                const double cb2 = power_of<2>(r1 - r2) * (15.0 - r1 * 10.0 + r12 + r2 * (-20.0 + r1 * 8.0) + r22 * 6.0);
                const double cb4 = (210.0 - r1  * 336.0 + r12 * 168.0 - r13 * 28.0 + r14)
                        + r2  * (-504.0 + r1 * 672.0 - r12 * 252.0 + r13 * 24.0)
                        + r22 * (420.0 - r1 * 420.0 + r12 * 90.0)
                        + r23 * (-140.0 + r1 * 80.0)
                        + r24 * 15.0;

                return (
                        (r1 - r2) * (L - 1.0 / r2) * (ca0 + ca2 * a2pi + ca4 * a4pi)
                        + (r1 - 1.0) * (1.0 / r2 - 1.0) * (r2 - r1) * (cb0 + cb2 * a2pi + cb4 * a4pi)
                        + (1.0 - r1) * (r1 - 1.0) * (L - 1.0) * (cb0 + cb2 * a2pi + cb4 * a4pi)
                    ) * (r1 - 1.0) * 3.0 / power_of<8>(r1 - r2);
            };
            auto T1tw2thetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1, r16 = r13 * r13;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2, r26 = r23 * r23, r27 = r24 * r23,
                      r28 = r24 * r24;
                const double Lr2 = std::log(r2), Lr2m1 = std::log(r2 - 1.0), Lmu = std::log(mc2 / (mu * mu));

                const double ca00 = (-r1 * 4. + r12 * 4.)
                    + r2  * (3. + r1 * 12. - r12 * 12.)
                    + r22 * (-13. - r1 * 4. + r12 * 8.)
                    + r23 * (13. - r1 * 4.)
                    - r24 * 3.;
                const double ca0mu = r2 * (1. - r1 * 3. + r12 * 2.)
                    + r22 * (r1 * 2. - r12 * 2.)
                    + r23 * (-1. + r1);
                const double ca0r2 = r2 * (-1. + r12)
                    + r22 * (3. - r1 * 4. + r12);
                const double ca0r2m1 = 2.0 * ca0mu;

                const double ca20 = (r1 * 1680. - r12 * 3120 + r13 * 1728 - r14 * 288.)
                    + r2 * (-1500. - r1 * 8675. + r12 * 17308. - r13 * 8208. + r14 * 864.)
                    + r22 * (10895. + r1 * 2160. - r12 * 21084. + r13 * 10080. - r14 * 576.)
                    + r23 * (-19396. + r1 * 15264. + r12 * 5412. - r13 * 3600.)
                    + r24 * (12516. - r1 * 12880. + r12 * 1484.)
                    + r25 * (-2576. + r1 * 2451.)
                    + r26 * 61.;
                const double ca2mu = r2 * (-180. + r1 * 1740. - r12 * 2712. + r13 * 1296. - r14 * 144.)
                    + r22 * (-840. - r1 * 1536. + r12 * 4248. - r13 * 2016. + r14 * 144.)
                    + r23 * (2448. - r1 * 1944. - r12 * 1224. + r13 * 720.)
                    + r24 * (-1800. + r1 * 2112. - r12 * 312.)
                    + r25 * (372. - r1 * 372.);
                const double ca2r2 = r2 * (180. + r1 * 840. - r12 * 1728. + r13 * 720. - r14 * 72.)
                    + r22 * (-1740. + r1 * 1536. + r12 * 144. + r13 * 432. - r14 * 72.)
                    + r23 * (1992. - r1 * 2448. + r12 * 1512. - r13 * 576.)
                    + r24 * (-216. - r1 * 672. + r12 * 168.)
                    + r25 * (-300. + r1 * 300.);
                const double ca2r2m1 = 2.0 * ca2mu;

                const double ca40 = r1 * 98910. - r12 * 281610. + r13 * 294000. - r14 * 136500. + r15 * 27000. - r16 * 1800.
                    + r2  * (-92610. - r1 * 628467. + r12 * 2091411. - r13 * 2110325. + r14 * 869950. - r15 * 136800. + r16 * 5400.)
                    + r22 * (865977. - r1 * 51660. - r12 * 3323460. + r13 * 3765400. - r14 * 1417650. + r15 * 181800. - r16 * 3600.)
                    + r23 * (-2201451. + r1 * 2911860. + r12 * 894420. - r13 * 2358600. + r14 * 840450. - r15 * 72000.)
                    + r24 * (2437925. - r1 * 4042510. + r12 * 1372230. + r13 * 345800. - r14 * 156250.)
                    + r25 * (-1293760. + r1 * 2102595. - r12 * 890655. + r13 * 63725.)
                    + r26 * (307725. - r1 * 414708. + r12 * 137664.)
                    + r27 * (-23987. + r1 * 23980)
                    + r28 * 181.;
                const double ca4mu = r2 * (-6300. + r1 * 107730. - r12 * 271530. + r13 * 266700. - r14 * 115950. + r15 * 20250. - r16 * 900.)
                    + r22 * (-63630. - r1 * 103320. + r12 * 557550. - r13 * 603000. + r14 * 246600. - r15 * 35100. + r16 * 900.)
                    + r23 * (242550. - r1 * 299250. - r12 * 210600. + r13 * 411300. - r14 * 158850. + r15 * 14850.)
                    + r24 * (-304500. + r1 * 539400. - r12 * 200700. - r13 * 62400. + r14 * 28200.)
                    + r25 * (169650. - r1 * 304200. + r12 * 147150. - r13 * 12600.)
                    + r26 * (-40950. + r1 * 62820. - r12 * 21870.)
                    + r27 * (3180. - r1 * 3180.);
                const double ca4r2 = r2 * (6300. + r1 * 63630. - r12 * 204750. + r13 * 210000. - r14 * 87750. + r15 * 12600. - r16 * 450.)
                    + r22 * (-107730. + r1 * 103320. + r12 * 166950. - r13 * 237000. + r14 * 74250. + r15 * 3600. - r16 * 450.)
                    + r23 * (233730. - r1 * 425250. + r12 * 210600. - r13 * 45000. + r14 * 65700 - r15 * 10800.)
                    + r24 * (-172200. + r1 * 300600. - r12 * 165600. + r13 * 71400. - r14 * 23700.)
                    + r25 * (34050. - r1 * 16650. - r12 * 54900. + r13 * 8100.)
                    + r26 * (8100. - r1 * 38520. + r12 * 17820.)
                    + r27 * (-2730. + r1 * 2730.);
                const double ca4r2m1 = 2.0 * ca4mu;

                return -3.0 / (r2 * power_of<4>(r1 - r2)) * (ca00 + ca0mu * Lmu + ca0r2 * Lr2 + ca0r2m1 * Lr2m1)
                    + 1.0 / (4.0 * r2 * power_of<6>(r1 - r2)) * (ca20 + ca2mu * Lmu + ca2r2 * Lr2 + ca2r2m1 * Lr2m1) * a2pi
                    + 1.0 / (10.0 * r2 * power_of<8>(r1 - r2)) * (ca40 + ca4mu * Lmu + ca4r2 * Lr2 + ca4r2m1 * Lr2m1) * a4pi;
            };
            auto T1tw2delta = [&] (const double & r1, const double & r2) -> double
            {
                static const double pi = M_PI, pi2 = pi * pi;

                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r13 * r12;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r23 * r22, r26 = r23 * r23;
                const double L1mr1 = std::log(1.0 - r1), Lr2 = std::log(r2), Lr2m1 = std::log(r2 - 1.0), Lmu = std::log(mc2 / (mu * mu));
                const double L1mr12 = L1mr1 * L1mr1, Lr2m12 = Lr2m1 * Lr2m1;
                const double dilogr1 = real(dilog(complex<double>(r1, 0.0)));
                const double dilog1mr2 = real(dilog(complex<double>(1.0 - r2, 0.0)));

                const double ca00 = r2 * (18.0 + pi2 - r1 * (10.0 + pi2)) + r22 * (-10.0 - pi2 + r1 * (2.0 + pi2));
                const double ca0mu = r2 * (-15.0 + r1 * 9.0) + r22 * (9.0 - r1 * 3.0);
                const double ca0r1 = -2.0 + r1 * 2.0 + r2 * (4.0 - r1 * 4.0) + r22 * (-2.0 + r1 * 2.0);
                const double ca0r12 = r2 * (-2.0 + r1 * 2.0) + r22 * (2.0 - r1 * 2.0);

                const double ca20 = r2 * (5.0 * (34.0 + pi2) - r1 * 10.0 * (26.0 + pi2) + r12 * 6.0 * (18.0 + pi2) + r13 * (-10.0 - pi2))
                    + r22 * (-10.0 * (26.0 + pi2) + r1 * 18.0 * (18.0 + pi2) - r12 * 9.0 * (10.0 + pi2) + r13 * (2.0 + pi2))
                    + r23 * (6.0 * (18.0 + pi2) - r1 * 9.0 * (10.0 + pi2) + r12 * 3.0 * (2.0 + pi2))
                    + r24 * (-10.0 - pi2 + r1 * (2.0 + pi2));
                const double ca2mu = r2 * (-135.0 + r1 * 210.0 - r12 * 90.0 + r13 * 9.0)
                    + r22 * (210.0 - r1 * 270.0 + r12 * 81.0 - r13 * 3.0)
                    + r23 * (-90.0 + r1 * 81.0 - r12 * 9.0)
                    + r24 * (9.0 - r1 * 3.0);
                const double ca2r1 = -10.0 + r1 * 20.0 - r12 * 12.0 + r13 * 2.0
                    + r2  * (30.0 - r1 * 56.0 + r12 * 30.0 - r13 * 4.0)
                    + r22 * (-32.0 + r1 * 54.0 - r12 * 24.0 + r13 * 2.0)
                    + r23 * (14.0 - r1 * 20.0 + r12 * 6.0)
                    + r24 * (-2.0 + r1 * 2.0);
                const double ca2r12 = r2 * (-10.0 + r1 * 20.0 - r12 * 12.0 + r13 * 2.0)
                    + r22 * (20.0 - r1 * 36.0 + r12 * 18.0 - r13 * 2.0)
                    + r23 * (-12.0 + r1 * 18.0 - r12 * 6.0)
                    + r24 * (2.0 - r1 * 2.0);

                const double ca40 = r2 * (42.0 * (50.0 + pi2) - r1 * 126.0 * (42.0 + pi2) + r12 * 140.0 * (34.0 + pi2) - r13 * 70.0 * (26.0 + pi2) + r14 * 15.0 * (18.0 + pi2) + r15 * (-10.0 - pi2))
                    + r22 * (-126.0 * (42.0 + pi2) + r1 * 350.0 * (34.0 + pi2) - r12 * 350.0 * (26.0 + pi2) + r13 * 150.0 * (18.0 + pi2) - r14 * 25.0 * (10.0 + pi2) + r15 * (2.0 + pi2))
                    + r23 * (140.0 * (34.0 + pi2) - r1 * 350.0 * (26.0 + pi2) + r12 * 300.0 * (18.0 + pi2) - r13 * 100.0 * (10.0 + pi2) + r14 * 10.0 * (2.0 + pi2))
                    + r24 * (-70.0 * (26.0 + pi2) + r1 * 150.0 * (18.0 + pi2) - r12 * 100.0 * (10.0 + pi2) + r13 * 20.0 * (2.0 + pi2))
                    + r25 * (15.0 * (18.0 + pi2) - r1 * 25.0 * (10.0 + pi2) + r12 * 10.0 * (2.0 + pi2))
                    + r26 * (-10.0 - pi2 + r1 * (2.0 + pi2));
                const double ca4mu = r2 * (-1638.0 + r1 * 4158.0 - r12 * 3780.0 + r13 * 1470.0 - r14 * 225.0 + r15 * 9.0)
                    + r22 * (4158.0 - r1 * 9450.0 + r12 * 7350.0 - r13 * 2250.0 + r14 * 225.0 - r15 * 3.0)
                    + r23 * (-3780.0 + r1 * 7350.0 - r12 * 4500.0 + r13 * 900.0 - r14 * 30.0)
                    + r24 * (1470.0 - r1 * 2250.0 + r12 * 900.0 - r13 * 60.0)
                    + r25 * (-225.0 + r1 * 225.0 - r12 * 30.0)
                    + r26 * (9.0 - r1 * 3.0);
                const double ca4r1 = -84.0 + r1 * 252.0 - r12 * 280.0 + r13 * 140.0 - r14 * 30.0 + r15 * 2.0
                    + r2  * (336.0 - r1 * 952.0 + r12 * 980.0 - r13 * 440.0 + r14 * 80.0 - r15 * 4.0)
                    + r22 * (-532.0 + r1 * 1400.0 - r12 * 1300.0 + r13 * 500.0 - r14 * 70.0 + r15 * 2.0)
                    + r23 * (420.0 - r1 * 1000.0 + r12 * 800.0 - r13 * 240.0 + r14 * 20.0)
                    + r24 * (-170.0 + r1 * 350.0 - r12 * 220.0 + r13 * 40.0)
                    + r25 * (32.0 - r1 * 52.0 + r12 * 20.0)
                    + r26 * (-2.0 + r1 * 2.0);
                const double ca4r12 = r2 * (-84.0 + r1 * 252.0 - r12 * 280.0 + r13 * 140.0 - r14 * 30.0 + r15 * 2.0)
                    + r22 * (252.0 - r1 * 700.0 + r12 * 700.0 - r13 * 300.0 + r14 * 50.0 - r15 * 2.0)
                    + r23 * (-280.0 + r1 * 700.0 - r12 * 600.0 + r13 * 200.0 - r14 * 20.0)
                    + r24 * (140.0 - r1 * 300.0 + r12 * 200.0 - r13 * 40.0)
                    + r25 * (-30.0 + r1 * 50.0 - r12 * 20.0)
                    + r26 * (2.0 - r1 * 2.0);

                return -3.0 / (r2 * power_of<7>(r1 - r2)) * (
                            power_of<4>(r1 - r2) * (ca00 + ca0mu * Lmu + ca0r1 * (L1mr1 - 2.0 * Lr2m1) + ca0r12 * (L1mr12 + Lr2m12 - 2.0 * Lr2 * Lr2m1 + L1mr1 * (Lr2 - 2.0 * Lr2m1) + dilogr1 - 3.0 * dilog1mr2))
                            + 6.0 * power_of<2>(r1 - r2) * (ca20 + ca2mu * Lmu + ca2r1 * (L1mr1 - 2.0 * Lr2m1) + ca2r12 * (L1mr12 + Lr2m12 - 2.0 * Lr2 * Lr2m1 + L1mr1 * (Lr2 - 2.0 * Lr2m1) + dilogr1 - 3.0 * dilog1mr2)) * a2pi
                            + 15.0 * (ca40 + ca4mu * Lmu + ca4r1 * (L1mr1 - 2.0 * Lr2m1) + ca4r12 * (L1mr12 + Lr2m12 - 2.0 * Lr2 * Lr2m1 + L1mr1 * (Lr2 - 2.0 * Lr2m1) + dilogr1 - 3.0 * dilog1mr2)) * a4pi
                        );
            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    return -2.0 * (T1tw2thetarhom1(r1, r2) + T1tw2theta1mrho(r1, r2) + T1tw2delta(r1, r2))
                        * weight * std::exp(-mc2 * r2 / _M2);
                }
            );

            static const double eps = 1e-12;

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, 1.0 + eps, s0D(q2) / mc2, config);
        }

        double F_nlo_tw3(const double & q2, const double _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.

            static const double pi2 = M_PI * M_PI;

            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double r1 = q2 / mc2;
            const double lmu = 2.0 * std::log(mc / mu());

            const double mupi = pi.mupi(mu);

            auto T1tw3ptheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double l1 = std::log((r2 - r1) / (r2 - 1.0)), l2 = lmu + std::log((r2 - 1.0) * (r2 - 1.0) / r2);

                return (r1 - r2 * (1.0 + r1 + r2) * l2) * l1 / (r2 * (r1 - r2));
            };
            auto T1tw3pthetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double logr2 = std::log(r2);
                const double l1 = std::log((1.0 - r1) / (r2 - r1));
                const double dl1 = pi2 / 6.0 + std::real(dilog(1.0 / r2)) + logr2 * (logr2 - std::log(r2 - 1.0));
                const double dl2 = std::real(-dilog(r1 / r2) + dilog(r1) - 2.0 * dilog((r2 - 1.0) / (r1 - 1.0)))
                    - logr2 * logr2 / 2.0 + logr2 * std::log(r2 - r1) - 2.0 * std::log((r2 - r1) / (1.0 - r1)) * std::log(r2 - 1.0);

                return (
                        dl1 * (1.0 + r1 + r2) + dl2 * (4.0 * r1 - 1.0)
                        + ((r1 + r2) * (r2 - 1.0) + (r1 * (2.0 - 3.0 * r2) + r2) * logr2) / (2.0 * r2)
                        + l1 * (1.0 - 2.0 * r1 + lmu * (4.0 * r1 - 1.0))
                    ) / (r2 - r1);
            };

            auto T1tw3pdeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double dlr1 = std::real(dilog(complex<double>(r1, 0.0)));
                const double dl1mr2 = std::real(dilog(complex<double>(1.0 - r2, 0.0)));

                return (
                        6.0 - 2.0 * r1 - pi2 / 6.0 * (1.0 + 4.0 * r1)
                        + lr2 * (l1mr1 * r1 - lr2m1 * 2.0 * r1)
                        + lr2m1 * (lr2m1 * (1.0 + 2.0 * r1) - 4.0 + 2.0 * r1 * (r2 - 1.0) / r2 - l1mr1 * 2.0 * r1 + lmu * (1.0 + r1))
                        + lmu * 3.0 / 2.0 * (r1 - 3.0)
                        + l1mr1 * (-l1mr1 + 2.0 + r1 + r1 / r2 - (1.0 + r1) * lmu)

                        - dlr1 + (1.0 - 2.0 * r1) * dl1mr2
                    ) / (r2 - r1);
            };
            auto T1tw3sigmatheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr2mr1 = std::log(r2 - r1);

                return (
                        - 6.0 * (r1 * r1 + 2.0 * (r2 - 1.0) * r2 + r1 * (-1.0 + 2.0 * r2 - 2.0 * r2 * r2))
                            / (r2 * (r1 - r2) * (r1 - r2))
                        + lr2mr1 * ((lmu - lr2 + 2.0 * lr2m1) * 6.0 * (1.0 + r1 + r2) / (r1 - r2) - 6.0 * r1 / (r2 * (r1 - r2)))
                        + lr2m1 * ((-2.0 * lr2m1 - lmu + lr2) * 6.0 * (1.0 + r1 + r2) / (r1 - r2)
                            + 6.0 * (-2.0 * (r2 - 1.0) * r2 + r1 * r2 * (2.0 * r2 - 5.0) + r1 * r1 * (1.0 + 2.0 * r2))
                                / ((r2 - r1) * (r2 - r1) * r2)
                        )
                        + (lmu - lr2) * 6.0 * (r1 - 1.0) * (-1.0 + r1 + r2) / ((r2 - r1) * (r2 - r1))
                    ) / (r2 - r1);
            };
            auto T1tw3sigmathetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr2mr1 = std::log(r2 - r1);
                const double l1 = 2.0 * lr2m1 + lmu - lr2;
                const double dl1 = std::real(dilog(r1) - dilog(r1 / r2) - 2.0 * dilog((r2 - 1.0) / (r1 - 1.0)));
                const double dl2 = std::real(dilog(1.0 / r2)) - l1 * l1;

                return 3.0 * (
                        - dl1 * 2.0 * (4.0 * r1 - 1.0) * (r1 - r2) * r2
                        - dl2 * 2.0 * (r1 - r2) * r2 * (1.0 + r1 + r2)
                        + l1 * (
                            - l1 * (r1 - r2) * r2 * (5.0 + 4.0 * r2)
                            + lr2mr1 * 2.0 * (4.0 * r1 - 1.0) * (r1 - r2) * r2
                            - lr2m1 * 2.0 * (-5.0 + 5.0 * r1 - 3.0 * r2) * (r1 - r2) * r2
                            - lmu * 2.0 * (-3.0 + 2.0 * r1 - 2.0 * r2) * (r1 - r2) * r2
                            + r1 * (r2 - 1.0) * r2 - 5.0 * r2 * r2 + r1 * r1 * (2.0 + r2 - 2.0 * r2 * r2)
                        )
                        + lr2mr1 * (
                            - 2.0 * (-1.0 + 2.0 * r1) * (r1 - r2) * r2
                        )
                        + lr2m1 * (
                            lr2m1 * 4.0 * (r1 - r2) * (-2.0 + 3.0 * r1 - r2) * r2
                            - l1mr1 * 4.0 * (4.0 * r1 - 1.0) * (r1 - r2) * r2
                            + lmu * 2.0 * (-5.0 + 5.0 * r1 - 3.0 * r2) * (r1 - r2) * r2
                            - 2.0 * r1 * (-1.0 + r2) * r2 + 2.0 * r2 * (2.0 + 3.0 * r2) + r1 * r1 * (-4.0 - 2.0 * r2 + 4.0 * r2 * r2)
                        )
                        + l1mr1 * (
                            - lmu * 2.0 * (4.0 * r1 - 1.0) * (r1 - r2) * r2
                            + 2.0 * (-1.0 + 2.0 * r1) * (r1 - r2) * r2
                        )
                        + lmu * (
                            lmu * (-3.0 + 2.0 * r1 - 2.0 * r2) * (r1 - r2) * r2
                            -r1 * (r2 - 1.0) * r2 + r2 * (2.0 + 3.0 * r2) + r1 * r1 * (-2.0 + r2 * (-1.0 + 2.0 * r2))
                        )
                        + (
                            r2 * r2 * (pi2 - 3.0 + (3.0 + pi2) * r2)
                            + r1 * (6.0 - (6.0 + pi2) * r2)
                            - r1 * r1 * (3.0 + r2 * (pi2 - 9.0 + 6.0 * r2))
                        ) / 3.0
                    ) / (power_of<3>(r1 - r2) * r2);

            };
            auto T1tw3sigmadeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double l1 = 2.0 * lr2m1 + lmu - lr2;
                const double l2 = l1mr1 - 2.0 * lr2m1;
                const double dl1 = std::real(dilog(r1)) + l1mr1 * (l1mr1 + lmu);
                const double dl2 = std::real(dilog(1.0 - r2)) + lr2m1 * lr2m1;

                return (
                        dl1 * 6.0 * (r1 * (3.0 - 4.0 * r2) + r2)
                        + dl2 * (-30.0 * r2 + 6.0 * r1 * (-7.0 + 2.0 * r1 + 10.0 * r2))
                        + l1 * l2 * (-12.0 * r2 + 6.0 * r1 * (-2.0 + r1 + 3.0 * r2))
                        + lr2m1 * (
                            lmu * (-18.0 * r2 + 6.0 * r1 * (-5.0 + r1 + 7.0 * r2))
                            - 12.0 * (r2 + r1 * (2.0 - r1 - 3.0 * r2 + r2 * r2)) / r2
                        )
                        - l1mr1 * 6.0 * ((-2.0 + r1) * r1 - 2.0 * r2 + r1 * (5.0 + r1) * r2 + (2.0 - 5.0 * r1) * r2 * r2) / r2
                        + lmu * (-3.0 * r1 * (-17.0 + r1 - 5.0 * r2) + 9.0 * r2)
                        + r1 * (-72.0 + pi2 * (-5.0 + 4.0 * r1)) + r2 * (6.0 * (-1.0 + r1) * r1 + pi2 * (-7.0 + 8.0 * r1))
                        - 6.0 * (1.0 + 3.0 * r2)
                    ) / ((r1 - r2) * (r1 - r2) * (r1 - r2));
            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    return (
                            2.0 / (r2 - r1) * (T1tw3pthetarhom1(r1, r2) + T1tw3ptheta1mrho(r1, r2) + T1tw3pdeltarhom1(r1, r2))
                            + 1.0 / 3.0 * (T1tw3sigmathetarhom1(r1, r2) + T1tw3sigmatheta1mrho(r1, r2) + T1tw3sigmadeltarhom1(r1, r2))
                        ) * weight * std::exp(-mc2 * r2 / _M2);
                }
            );

// for diagnostics purposes only
#if 0
            std::function<double (const double &)> iT1tw3ptheta1mrho(
                [&] (const double & r2) -> double
                { return T1tw3ptheta1mrho(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
            std::function<double (const double &)> iT1tw3pthetarhom1(
                [&] (const double & r2) -> double
                { return T1tw3pthetarhom1(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
            std::function<double (const double &)> iT1tw3pdeltarhom1(
                [&] (const double & r2) -> double
                { return T1tw3pdeltarhom1(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
            std::function<double (const double &)> iT1tw3sigmatheta1mrho(
                [&] (const double & r2) -> double
                { return T1tw3sigmatheta1mrho(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
            std::function<double (const double &)> iT1tw3sigmathetarhom1(
                [&] (const double & r2) -> double
                { return T1tw3sigmathetarhom1(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
            std::function<double (const double &)> iT1tw3sigmadeltarhom1(
                [&] (const double & r2) -> double
                { return T1tw3sigmadeltarhom1(0.0, r2) * std::exp(-mc2 * r2 / _M2); }
            );
#endif

            static const double eps = 1e-12;

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * mc2;

            return fpi * mupi * mc * (
                    integrate<GSL::QAGS>(integrand, 1.0 + eps, s0D(q2) / mc2, config)
                    - (
                        2.0 / (1.0 - r1) * (4.0 - 3.0 * lmu)
                        + 2.0 * (1.0 + r1) / power_of<2>(1.0 - r1) * (4.0 - 3.0 * lmu)
                    ) * weight * std::exp(-mc2 / _M2)
                );
        }

        // expressions for the \tilde{F}

        double Ftil_lo_tw3_integrand(const double & u, const double & q2, const double _M2, const double _select_weight) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi;
            const double mupi = pi.mupi(mu);
            const double omega3pi = pi.omega3pi(mu);

            // auxilliary functions and their first derivatives
            auto I3til = [&] (const double & u) -> double
            {
                const double u2 = u * u, ubar2 = (1.0 - u) * (1.0 - u);

                return 5.0 / 2.0 * u2 * ubar2 * (28.0 * u2 * omega3pi - 2.0 * u * (17.0 * omega3pi + 12.0) + 9.0 * (omega3pi + 4.0));
            };
            auto I3til_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u;

                return 15.0 * u * (u - 1.0) * (28.0 * u3 * omega3pi - u2 * (47.0 * omega3pi + 20.0) + u * (23.0 * omega3pi + 36.0) - 3.0 * (omega3pi + 4.0));
            };

            const double u2 = u * u;
            const double tw3a = pi.phi3p(u, mu) / u
                + 1 / (6 * u) * pi.phi3s_d1(u, mu);
            const double tw3b = mpi2 / (mc2 - q2 + u2 * mpi2)
                * (I3til_d1(u) - (2.0 * u * mpi2) / (mc2 - q2 + u2 * mpi2) * I3til(u));

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

            return std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) * weight
                * (mupi / mc * tw3a + pi.f3pi(mu) / (mc * fpi) * tw3b);
        }

        double Ftil_lo_tw3(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0tilD(q2) - q2));

            std::function<double (const double &)> integrand(std::bind(&Implementation<AnalyticFormFactorDToPiKKMO2009>::Ftil_lo_tw3_integrand, this, std::placeholders::_1, q2, _M2, _select_weight));

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, u0, 1.000, config);
        }

        double Ftil_lo_tw4(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi, mpi4 = mpi2 * mpi2;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0tilD(q2) - q2));
            const double a2pi = pi.a2pi(mu);
            const double deltapipi = pi.deltapipi(mu);
            const double omega4pi = pi.omega4pi(mu);

            // auxilliary functions and their first derivatives
            auto I4bar = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u;
                const double ubar = 1.0 - u;

                return 1.0 / 48.0 * u * ubar * (
                        mpi2 * (
                            -(54.0 * u3 - 81.0 * u2 - 27.0 * u + 27.0)
                            + 27.0 * a2pi * (32.0 * u3 - 43.0 * u2 + 11.0 * u + 1.0)
                        )
                        - 20.0 * u * (
                            (12.0 - 20.0 * u)
                            + (378.0 * u2 - 567.0 * u + 189.0) * omega4pi
                        ) * deltapipi
                    );
            };
            auto I4barI = [&] (const double & u) -> double
            {
                const double u2 = u * u;
                const double ubar = 1.0 - u, ubar2 = ubar * ubar;

                return 1.0 / 96.0 * u2 * ubar2 * (
                        mpi2 * (
                            9.0 * (3.0 + 2.0 * ubar * u)
                            + 9.0 * a2pi * (32.0 * u2 - 26.0 * u - 3.0)
                        )
                        + 40.0 * u * (4.0 + 63.0 * ubar * omega4pi) * deltapipi
                    );
            };
            auto I4bar_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u, u4 = u2 * u2;

                return 1.0 / 48.0 * (
                        27.0 * mpi2 * (
                            (10.0 * u4 - 20.0 * u3 + 6.0 * u2 + 4.0 * u - 1.0)
                            - a2pi * (160.0 * u4 - 300.0 * u3 + 162.0 * u2 - 20.0 * u - 1.0)
                        )
                        + 40.0 * u * (
                            (-40.0 * u2 + 48.0 * u - 12.0)
                            + 189.0 * (5.0 * u3 - 10.0 *  u2 + 6.0 * u - 1.0) * omega4pi
                        ) * deltapipi
                    );
            };
            std::function<double (const double &)> integrand(
                [&] (const double & u) -> double
                {
                    const double u2 = u * u;

                    const double tw4psi = pi.psi4(u, mu) - (2.0 * u * mpi2) / (mc2 - q2 + u2 * mpi2) * pi.psi4_i(u, mu);
                    const double tw4I4bar = (- I4bar_d1(u) + (6.0 * u * mpi2) / (mc2 - q2 + u2 * mpi2) * I4bar(u) + (12.0 * u2 * mpi4) / power_of<2>(mc2 - q2 + u2 * mpi2) * I4barI(u)) * 2.0 * u * mpi2 / (mc2 - q2 + u2 * mpi2);

                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

                    return std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) * weight
                            * (tw4psi + tw4I4bar) / (mc2 - q2 + u2 * mpi2);
                }
            );

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, u0, 1 - 1e-10, config);
        }

        double Ftil_nlo_tw2(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double a2pi = pi.a2pi(mu), a4pi = pi.a4pi(mu);
            const double r1 = q2 / mc2;

            // imaginary parts of the hard scattering kernel, integrated over rho.
            auto T1tiltw2theta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1, r16 = r13 * r13;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2;

                const double ca0 = -r1 + 2.0 * r12 - r13 
                    + r2 * (1.0 - r1 - r12 + r13)
                    + r22 * (-1.0 + 2.0 * r1 - r12);
                const double ca2 = -15.0 + 40.0 * r1 - 36.0 * r12 + 12.0 * r13 - r14
                    + r2 * (35.0 - 88.0 * r1 + 72.0 * r12 - 20.0 * r13 + r14)
                    + r22 * (-26.0 + 60.0 * r1 - 42.0 * r12 + 8.0 * r13)
                    + r23 * (6.0 - 12.0 * r1 + 6.0 * r12);
                const double ca4 = -210.0 + 756.0 * r1 - 1050.0 * r12 + 700.0 * r13 - 225.0 * r14 + 30.0 * r15 - r16
                    + r2 * (714.0 - 2436.0 * r1 + 3150.0 * r12 - 1900.0 * r13 + 525.0 * r14 - 54.0 * r15 + r16)
                    + r22 * (-924.0 + 2940.0 * r1 - 3450.0 * r12 + 1800.0 * r13 - 390.0 * r14 + 24.0 * r15)
                    + r23 * (560.0 - 1620.0 * r1 + 1650.0 * r12 - 680.0 * r13 + 90.0 * r14)
                    + r24 * (-155.0 + 390.0 * r1 - 315.0 * r12 + 80.0 * r13)
                    + r25 * (15.0 - 30.0 * r1 + 15.0 * r12);

                return -6.0 / (r2 * power_of<7>(r1 - r2)) * (
                            power_of<3>(r1 - r2) * ca0 + power_of<2>(r1 - r2) * ca2 * a2pi + ca4 * a4pi
                        );

            };
            auto T1tiltw2thetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2, r26 = r23 * r23, r27 = r24 * r23;
                const double Lr2 = std::log(r2);

                const double ca00 = 1 - 2.0 * r1
                    + r2 * (-1.0 + 4.0 * r1)
                    + r22 * (-1.0 - 2.0 * r1)
                    + r23;
                const double ca0r2 = -r2 * r1 + r22 * (1.0 + r1) - r23;

                const double ca20 = (15.0 - 40.0 * r1 + 36.0 * r12 - 12.0 * r13)
                    + r2 * (-35.0 + 93.0 * r1 - 87.0 * r12 + 24.0 * r13)
                    + r22 * (21.0 - 45.0 * r1 + 96.0 * r12 - 12.0 * r13)
                    + r23 * (-6.0 - 29.0 * r1 - 45.0 * r12)
                    + r24 * (-16.0 + 21.0 * r1)
                    + r25 * (21.0);
                const double ca2r2 = r2 * (-6 * r13)
                    + r22 * (6.0 * r13 + 18.0 * r12)
                    + r23 * (12.0 * r1 + 12.0 * r12)
                    + r24 * (-24.0 - 12.0 * r1)
                    + r25 * (-6.0);

                const double ca40 = 420.0 - 1512.0 * r1 + 2100.0 * r12 - 1400.0 * r13 + 450.0 * r14 - 60.0 * r15
                    + r2  * (-1428.0 + 4935.0 * r1 - 6510.0 * r12 + 4080.0 * r13 - 1260.0 * r14 + 120.0 * r15)
                    + r22 * (1785.0 - 5775.0 * r1 + 6900.0 * r12 - 3600.0 * r13 + 1590.0 * r14 - 60.0 * r15)
                    + r23 * (-1015.0 + 2820.0 * r1 - 2040.0 * r12 + 2240.0 * r13 - 780.0 * r14)
                    + r24 * (450.0 - 1200.0 * r1 - 1080.0 * r12 - 1320.0 * r13)
                    + r25 * (-660.0 - 243.0 * r1 + 630.0 * r12)
                    + r26 * (313.0 + 975.0 * r1)
                    + r27 * (135.0);
                const double ca4r2 = r2 * (-15.0 * r15)
                    + r22 * (75.0 * r14 + 15.0 * r15)
                    + r23 * (690.0 * r13 + 135.0 * r14)
                    + r24 * (150.0 * r12 + 150.0 * r13)
                    + r25 * (-705.0 * r1 - 150.0 * r12)
                    + r26 * (-195.0 - 135.0 * r1)
                    + r27 * (-15.0);

                return -6.0 / (r2 * power_of<7>(r1 - r2)) * (power_of<4>(r1 - r2) * (ca00 + ca0r2 * Lr2)
                        + power_of<2>(r1 - r2) * (ca20 + ca2r2 * Lr2) * a2pi
                        + (ca40 / 2.0 + ca4r2 * Lr2) * a4pi
                        );
            };
            auto T1tiltw2delta = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r13 * r12, r16 = r13 * r13, r17 = r14 * r13;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r23 * r22, r26 = r23 * r23;
                const double L1mr1 = std::log(1.0 - r1);

                const double ca00 = r1 - r12 + r2 * (-1.0 + r12) + r22 * (1.0 - r1);
                const double ca0r1 = r1 - 2.0 * r12 + r13
                    + r2 * (-1.0 + r1 + r12 - r13)
                    + r22 * (1.0 - 2 * r1 + r12);

                const double ca20 = 5.0 * r1 - 10.0 * r12 + 6.0 * r13 - r14
                    + r2 * (-5.0 + 12.0 * r12 - 8.0 * r13 + r14)
                    + r22 * (10.0 - 12.0 * r1 + 2.0 * r13)
                    + r23 * (-6.0 + 8.0 * r1 - 2.0 * r12)
                    + r24 * (1.0 - r1);
                const double ca2r1 = 5.0 * r1 - 15.0 * r12 + 16.0 * r13 - 7.0 * r14 + r15
                    + r2 * (-5.0 + 5.0 * r1 + 12.0 * r12 - 20.0 * r13 + 9.0 * r14 - r15)
                    + r22 * (10.0 - 22.0 * r1 + 12.0 * r12 + 2.0 * r13 - 2.0 * r14)
                    + r23 * (-6.0 + 14.0 * r1 - 10.0 * r12 + 2.0 * r13)
                    + r24 * (1.0 - 2.0 * r1 + r12);

                const double ca40 = 42.0 * r1 - 126.0 * r12 + 140.0 * r13 - 70.0 * r14 + 15.0 * r15 - r16
                    + r2 * (-42.0 + 210.0 * r12 - 280.0 * r13 + 135.0 * r14 - 24.0 * r15 + r16)
                    + r22 * (126.0 - 210.0 * r1 + 150.0 * r13 - 75.0 * r14 + 9.0 * r15)
                    + r23 * (-140.0 + 280.0 * r1 - 150.0 * r12 + 10.0 * r14)
                    + r24 * (70.0 - 135.0 * r1 + 75.0 * r12 - 10.0 * r13)
                    + r25 * (-15.0 + 24.0 * r1 - 9.0 * r12)
                    + r26 * (1.0 - r1);
                const double ca4r1 = 42.0 * r1 - 168.0 * r12 + 266.0 * r13 - 210.0 * r14 + 85.0 * r15 - 16.0 * r16 + r17
                    + r2 * (-42.0 + 42.0 * r1 + 210.0 * r12 - 490.0 * r13 + 415.0 * r14 - 159.0 * r15 + 25.0 * r16 - r17)
                    + r22 * (126.0 - 336.0 * r1 + 210.0 * r12 + 150.0 * r13 - 225.0 * r14 + 84.0 * r15 - 9.0 * r16)
                    + r23 * (-140.0 + 420.0 * r1 - 430.0 * r12 + 150.0 * r13 + 10.0 * r14 - 10.0 * r15)
                    + r24 * (70.0 - 205.0 * r1 + 210.0 * r12 - 85.0 * r13 + 10.0 * r14)
                    + r25 * (-15.0 + 39.0 * r1 - 33.0 * r12 + 9.0 * r13)
                    + r26 * (1.0 - 2.0 * r1 + r12);

                return -6.0 / (r1 * r1 * power_of<7>(r1 - r2)) * (
                            power_of<4>(r1 - r2) * (ca00 * r1 + ca0r1 * L1mr1)
                            + 6.0 * power_of<2>(r1 - r2) * (ca20 * r1 + ca2r1 * L1mr1) * a2pi
                            + 15.0 * (ca40 * r1 + ca4r1 * L1mr1) * a4pi
                        );

            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    return (T1tiltw2theta1mrho(r1, r2) + T1tiltw2thetarhom1(r1, r2) + T1tiltw2delta(r1, r2))
                       * weight * std::exp(-mc2 * r2 / _M2);
                }
            );

            static const double eps = 1e-12;

            return mc2 * fpi * integrate<GSL::QAGS>(integrand, 1.0 + eps, s0tilD(q2) / mc2, config);
        }

        double Ftil_nlo_tw3(const double & q2, const double _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.

            static const double pi2 = M_PI * M_PI;

            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double r1 = q2 / mc2;
            const double lmu = 2.0 * std::log(mc / mu());

            const double mupi = pi.mupi(mu);

            auto T1tiltw3ptheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double l1 = std::log((r2 - 1.0)/(r2 - r1)), l2 = lmu + std::log((r2 - 1.0) * (r2 - 1.0) / r2);

                return 2.0 * l1 * (r2 * l2 - 1.0);
            };
            auto T1tiltw3pthetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double logr1 = std::log(std::abs(r1));
                const double logr2 = std::log(r2);
                const double log1mr1 = std::log(1.0 - r1);
                const double logr2m1 = std::log(r2 - 1.0);
                const double logr2mr1 = std::log(r2 - r1);
                const double dl1 = (-1.0 - 5.0 * pi2 / 3.0  + 2.0 * (std::real(dilog(1.0 / r2)) + 2.0 * std::real(dilog(1.0 / r1)) + 2.0 * std::real(dilog(r2)) - 2.0 * std::real(dilog(r2 / r1)) + 4.0 * std::real(dilog((r2 - 1.0) / (r1 - 1.0))))) * r1 * r2 + r1;
                const double dl2 = ((3.0 + 4.0 * logr1 + 2.0 * logr2m1 - 4.0 * logr2mr1)* r1 - 2.0) * r2 - 2.0 * r1;
                const double dl3 = 8.0 * (logr2mr1 - log1mr1) * r1 * r2;
                const double dl4 = 2.0 * ((1.0 - 2.0 * lmu) * r1 - 1.0) * r2;
                const double dl5 = 2.0 * ((-1.0 + 2.0 * lmu) * r1 + 1.0) * r2;
                return (dl1 + dl2 * logr2 + dl3 * logr2m1 + dl4 * log1mr1 + dl5 * logr2mr1) / r1;
            };

            auto T1tiltw3pdeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1;
                const double logr2 = std::log(r2);
                const double logr2m1 = std::log(r2 - 1.0);
                const double log1mr1 = std::log(1.0 - r1);
                const double l1 = std::log((r2 - 1.0)/(1.0 - r1));
                const double dl1 = (3.0 + 4.0 * pi2 / 3.0 - 2.0 * lmu + 4.0 * std::real(dilog(1.0 - r2))) * r12 * r2 + r1 * r2;
                const double dl2 = (-2.0 * r12 + (1.0 - 2.0 * r1 + r12) * r2);
                const double dl3 = (4.0 - (6.0 + 4.0 * l1) * r2) * r12;
                const double dl4 = 2.0 * r12 * r2 * (logr2m1 + l1);
                const double dl5 = 2.0 * r12 * r2 * (1 - lmu);
                return (dl1 + dl2 * log1mr1 + dl3 * logr2m1 + dl4 * logr2 + dl5 * l1) / r12;
            };
            auto T1tiltw3sigmatheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr2mr1 = std::log(r2 - r1);

                return - 6.0 * ((r1 - r2) * (lr2mr1 - lr2m1) + r1 - 1.0) * (r2 * (lmu + 2.0 * lr2m1 - lr2) - 1.0);
            };
            auto T1tiltw3sigmathetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r22 = r2 * r2;
                const double lr1 = std::log(std::abs(r1)), l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr2mr1 = std::log(r2 - r1);
                const double dil = -2.0 * (2.0 * std::real(dilog(1/r1)) + 4.0 * std::real(dilog((r2 - 1.0)/(r1 - 1.0))) + std::real(dilog(1/r2)) + 2.0 * std::real(dilog(r2)) - 2.0 * std::real(dilog(r2 / r1)) + 4.0 * std::log((r1 - r2)/(r1 - 1)) * std::log(r2 - 1.0)) * (r2 - r1) * r2;
                const double dl1 = -(r2 - 1.0) * (2.0 - r2 + r1 * (-1.0 + 2.0 * r2));
                const double dl2 = ((r12 * (r2 - 2.0) - r1 * (r2 - 2.0) * r2 + 2.0 * r22) / r1 + 2.0 * (r2 - r1) * r2 * (2.0 * (lr2mr1 - lr1) - lr2m1)) * lr2;
                const double dl3 = -2.0 * (r1 - 1.0) * r2 * (r2 - r1) * l1mr1 / r1;
                const double dl4 = 2.0 * (r1 - 1.0) * r2 * (r2 - r1) * lr2mr1 / r1;
                const double dl5 = 4.0 * (l1mr1 - lr2mr1) * (r2 - r1) * r2;
                const double dl6 = 5.0 * (r2 - r1) * r2 / 3.0;

                return 3.0 * (dl1 + dl2 + dl3 + dl4 + dl5 * lmu + pi2 * dl6 + dil);
            };
            auto T1tiltw3sigmadeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r22 = r2 * r2;
                const double l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double dl1 = (- 17.0 * r1 - r12 + (1.0 - r1 + 2.0 * r12) * r2) / r1;
                const double dl2 = 2.0 * (2.0 * r1 + r2 - 3.0) / 3.0;
                const double dl3 = -4.0 * (-2.0 + r1 + r2) * (-1.0 + r2 * (2.0 * lr2m1 - lr2)) * lr2m1;
                const double dl4 = (4.0 * r12 - 2.0 * r13 + (-r13 - 4.0 * r12 + r1) * r2 + (3.0 * r12 - 2.0 * r1 + 1.0) * r22 
                        + 2.0 * r12 * r2 * (-2.0 + r1 + r2) * (2.0 * lr2m1 - lr2)) * l1mr1 / r12;
                const double dl5 = -4.0 * (r2 - 1.0) * l1mr1 * l1mr1 + 4.0 * (r1 + 2.0 * r2 - 3.0) * lr2m1 * lr2m1;
                const double dl6 = 2.0 * (5.0 + r2 - (l1mr1 - lr2m1) * (r2 - r1));
                const double dl7 = 4.0 * (-3.0 + r1 + 2.0 * r2) * std::real(dilog(1.0 - r2)) - 4.0 * (r2 - 1.0) * std::real(dilog(r1));

                return 3.0 * ((dl1 + pi2 * dl2 + dl5 + dl6 * lmu + dl7) * r2 + dl3 + dl4);
            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    try
                    {
                    return (
                            1.0 / (r2 * (r2 - r1)) * (T1tiltw3pthetarhom1(r1, r2) + T1tiltw3ptheta1mrho(r1, r2) + T1tiltw3pdeltarhom1(r1, r2))
                            + 1.0 / (3.0 * r2 * power_of<2>(r2 - r1)) * (T1tiltw3sigmatheta1mrho(r1, r2) + T1tiltw3sigmathetarhom1(r1, r2) + T1tiltw3sigmadeltarhom1(r1, r2))
                        ) * weight * std::exp(-mc2 * r2 / _M2);
                    }
                    catch (...)
                    {
                        throw InternalError("could not evaluate integrand of Ftil_nlo_tw3; r2 = " + stringify(s0tilD(q2) / mc2));
                    }
                }
            );

            static const double eps = 1e-12;

            try
            {
                return fpi * mupi * mc * integrate<GSL::QAGS>(integrand, 1.0 + eps, s0tilD(q2) / mc2, config);
            }
            catch (...)
            {
                throw InternalError("could not integrate Ftil_nlo_tw3; r2 = " + stringify(s0tilD(q2) / mc2));
            }
        }

        double FT_lo_tw2_integrand(const double & u, const double & q2, const double & _M2, const double & _select_weight) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi;

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

            return weight * std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) / u * this->pi.phi(u, mu);
        }

        double FT_lo_tw2(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0TD(q2) - q2));

            std::function<double (const double &)> integrand(std::bind(&Implementation<AnalyticFormFactorDToPiKKMO2009>::FT_lo_tw2_integrand, this, std::placeholders::_1, q2, _M2, _select_weight));

            return mc * fpi * integrate<GSL::QAGS>(integrand, u0, 1.000, config);
        }

        double FT_lo_tw3_integrand(const double & u, const double & q2, const double & _M2, const double & _select_weight) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi;
            const double mupi = pi.mupi(mu);
            const double u2 = u * u;

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

            return - mc * mupi * weight * std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2))
                * (pi.phi3s_d1(u, mu) - 2 * u * mpi2 * pi.phi3s(u, mu) / (mc2 - q2 + u2 * mpi2)) / (3.0 * (mc2 - q2 + u2 * mpi2));
        }

        double FT_lo_tw3(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0TD(q2) - q2));

            std::function<double (const double &)> integrand(std::bind(&Implementation<AnalyticFormFactorDToPiKKMO2009>::FT_lo_tw3_integrand, this, std::placeholders::_1, q2, _M2, _select_weight));

            return mc * fpi * integrate<GSL::QAGS>(integrand, u0, 1.000, config);
        }

        double FT_lo_tw4(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc, mpi2 = mpi * mpi, mpi4 = mpi2 * mpi2;
            const double u0 = std::max(1e-10, (mc2 - q2) / (s0TD(q2) - q2));
            const double a2pi = pi.a2pi(mu);
            const double deltapipi = pi.deltapipi(mu);
            const double omega4pi = pi.omega4pi(mu);

            // auxilliary functions and their first derivatives
            auto I4T = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u, u4 = u2 * u2, u5 = u4 * u;
                const double ubar = 1.0 - u, ubar2 = ubar * ubar;

                return 1.0 / 40.0 * (
                        mpi2 * (
                            + (90.0 * u5 - 225.0 * u4 + 90.0 * u3 + 90.0 * u2 - 45.0 * u)
                            + 9.0 * a2pi * (70.0 * u5 - 227.0 * u4 + 254.0 * u3 - 94.0 * u2 - 3.0 * u + 16.0 * (6.0 * u2 - 15.0 * u + 10.0) * u3 * std::atanh(1 - 2.0 * u) - 8.0 * std::log(ubar))
                        )
                        + 10.0 * (
                            40.0 * u2 * ubar2
                            - 21.0 * (-40.0 * u5 + 87.0 * u4 - 54.0 * u3 + 9.0 * u2 - 2.0 * u + 4.0 * (6.0 * u2 - 15.0 * u + 10.0) * u3 * std::atanh(1 - 2.0 * u) - 2.0 * std::log(ubar)) * omega4pi
                        ) * deltapipi
                    );
            };
            auto I4T_d1 = [&] (const double & u) -> double
            {
                const double u2 = u * u, u3 = u2 * u, u4 = u3 * u;
                const double ubar = 1.0 - u, ubar2 = ubar * ubar;

                return 1.0 / 8.0 * (
                        mpi2 * (
                            + (90.0 * u4 - 180.0 * u3 + 54.0 * u2 + 36.0 * u - 9.0)
                            + 9.0 * a2pi * (70.0 * u4 - 172.0 * u3 + 138.0 * u2 - 36.0 * u + 1.0 + 96.0 * ubar2 * u2 * std::atanh(1 - 2.0 * u))
                        )
                        + 40.0 * u * (
                            4.0 * (1.0 - 3.0 * u + 2.0 * u2)
                            + 21.0 * ubar * (-1.0 + 8.0 * u - 10.0 * u2 - 6.0 * ubar * u * std::atanh(1 - 2.0 * u)) * omega4pi
                        ) * deltapipi
                    );
            };
            std::function<double (const double &)> integrand(
                [&] (const double & u) -> double
                {
                    const double u2 = u * u;

                    const double tw4phi1 = (pi.phi4_d1(u, mu) - 2 * u * mpi2 * pi.phi4(u, mu) / (mc2 - q2 + u2 * mpi2)) / 4.0;
                    const double tw4phi2 = - mc2 * u * (pi.phi4_d2(u, mu) - 6.0 * u * mpi2 * pi.phi4_d1(u, mu) / (mc2 - q2 + u2 * mpi2) + 12.0 * u * mpi4 * pi.phi4(u, mu) / power_of<2>(mc2 - q2 + u2 * mpi2)) 
                        / (4.0 * (mc2 - q2 + u2 * mpi2));
                    const double tw4I4T = - (I4T_d1(u) - 2.0 * u * mpi2 * I4T(u) / (mc2 - q2 + u2 * mpi2));

                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * (mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / u;

                    return weight * std::exp(-(mc2 - q2 * (1.0 - u) + mpi2 * u * (1.0 - u)) / (u * _M2)) *
                            (tw4phi1 + tw4phi2 + tw4I4T) / (mc2 - q2 + u2 * mpi2);
                }
            );

            return mc * fpi * integrate<GSL::QAGS>(integrand, u0, 1 - 1e-10, config);
        }

        double FT_nlo_tw2(const double & q2, const double & _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double a2pi = pi.a2pi(mu), a4pi = pi.a4pi(mu);
            const double r1 = q2 / mc2;

            // imaginary parts of the hard scattering kernel, integrated over rho.
            auto T1Ttw2theta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2;
                const double L = std::log(power_of<2>(r2 - 1.0) * mc2 / (mu * mu * r2));

                const double ca0 = power_of<4>(r1 - r2) * (-r1 * 2.0 + r2 * (1.0 + r1));
                const double ca2 = power_of<2>(r1 - r2) * (-2.0 * (r1 * 55.0 - r12 * 65.0 + 16.0 * r13)
                        + r2 * (95.0 - r1 * 15.0 - r12 * 45.0 + r13)
                        + r22 * 2.0 * (-35.0 + r1 * 13.0 + r12 * 4.0)
                        + r23 * 6.0 * (1.0 + r1));
                const double ca4 = (-2877.0 * r1 + 6258.0 * r12 - r13 * 4592.0 + r14 * 1288.0 - r15 * 107.0)
                        + r2  * (2667.0 - r1 * 462.0 - r12 * 5502.0 + r13 * 4228.0 - r14 * 782.0 + r15)
                        + r22 * 6.0 * (-791.0 + r1 * 889.0 - r12 * 21.0 - r13 * 131.0 + r14 * 4.0)
                        + r23 * 10.0 * (266.0 - r1 * 280.0 + r12 * 35.0 + r13 * 9.0)
                        + r24 * 10.0 * (-49.0 + r1 * 26.0 + r12 * 8.0)
                        + r25 * 15.0 * (1.0 + r1);

                const double cb0 = power_of<4>(r1 - r2) * (-1.0 - r1 + 2.0 * r2);
                const double cb2 = power_of<2>(r1 - r2) * (-15.0 - r1 * 85.0 + r12 * 119.0 - r13 * 31.0 
                        + r2 * 2.0 * (65.0 - r1 * 34.0 - r12 * 13.0)
                        + r22 * 12.0 * (-8.0 + r1 * 5.0)
                        + r23 * 12.0);
                const double cb4 = (-210.0 - r1 * 2331.0 + r12 * 5754.0 - r13 * 4396.0 + r14 * 1259.0 - r15 * 106.0)
                        + r2  * 3.0 * (1127.0 - r1 * 728.0 - r12 * 1358.0 + r13 * 1252.0 - r14 * 243.0)
                        + r22 * 30.0 * (-189.0 + r1 * 245.0 - r12 * 52.0 - r13 * 14.0)
                        + r23 * 20.0 * (161.0 - r1 * 193.0 + 47.0 * r12)
                        + r24 * 15.0 * (-43.0 + 33.0 * r1)
                        + r25 * 30.0;

                return - (
                        ca0 + ca2 * a2pi + ca4 * a4pi - L * r2 * (cb0 + cb2 * a2pi + cb4 * a4pi)
                       ) * (r1 - 1.0) * (r2 - 1.0) * 3.0 / (power_of<8>(r1 - r2) * r2);
            };
            auto T1Ttw2thetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r14 * r1, r16 = r13 * r13;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r24 * r2, r26 = r23 * r23, r27 = r24 * r23;
                const double Lr2 = std::log(r2), Lr2m1 = std::log(r2 - 1.0), Lmu = std::log(mc2 / (mu * mu));

                const double C0 = r2 - 1.0;
                const double Clr2 = 60.0 * r2;
                const double Cl = 60.0 * (r1 - 1.0) * (r2 - 1.0) * r2;

                const double ca00 = -60.0 * (r1 * 2.0
                    + r2  * (-1.0 - r1 * 12.0 + r12 * 4.0)
                    + r22 * 2.0 * (5.0 - r1)
                    + r23 * (-1.0));
                const double ca0mu = -1.0 + 2.0 * r1 - r2;
                const double ca0r2 = 1.0 + r12 + r2 * (-3.0 - r1 * 2.0 - r12 * 3.0) + r22 * (4.0 + r1 * 2.0);
                const double ca0r2m1 = 2.0 * ca0mu;

                const double ca20 = -5.0 * (24.0 * (r1 * 55.0 - r12 * 90.0 + r13 * 36.0)
                    + r2 * (-1140.0 - r1 * 7475.0 + r12 * 13780.0 - r13 * 5544.0 + r14 * 288.0)
                    + r22 * (8915.0 - r1 * 3467.0 - r12 * 8672.0 + r13 * 2520.0)
                    + r23 * (-10097.0 + r1 * 10501.0 - r12 * 836.0)
                    + r24 * 5.0 * (-351.0 * r1 + 599.0)
                    + r25 * (-37.0));
                const double ca2mu = -15.0 + r1 * 130.0 - r12 * 96.0 + r13 * 12.0
                    + r2 * (-85.0 - r1 * 68.0 + r12 * 60)
                    + r22 * (119.0 - r1 * 26.0)
                    + r23 * (-31.0);
                const double ca2r2 = 15.0 + r1 * 70.0 - r12 * 144.0 + r13 * 60.0 + r14 * 6.0
                    + r2 * (-145.0 + r1 * 128.0 + r12 * 12.0 - r13 * 24.0 - r14 * 18.0)
                    + r22 * (166.0 - r1 * 204.0 + r12 * 54.0 - r13 * 72.0)
                    + r23 * (-18.0 + r1 * 40.0 + r12 * 38.0)
                    + r24 * (-1.0 + r1 * 37.0);
                const double ca2r2m1 = 2.0 * ca2mu;

                const double ca40 = 2.0 * (-30.0 * (r1 * 2877.0 - r12 * 7875.0 + r13 * 7700.0 - r14 * 3150.0 + r15 * 450.0)
                    + r2  * (80010.0 + r1 * 544677.0 - r12 * 1770111.0 - 25.0 * (- r13 * 69041.0 + 2.0 * (r14 * 13331.0 - r15 * 1746.0 + r16 * 36.0)))
                    + r22 * (-743127.0 + r1 * 499947.0 + r12 * 1581699.0 - 25.0 * (r13 * 78527.0 - r14 * 27488.0 + r15 * 1944.0))
                    + r23 * (1406664.0 - r1 * 2265963.0 + r12 * 539679.0 + 25.0 * (r13 * 19705.0 - r14 * 4702.0))
                    + r24 * (-1010261.0 + r1 * 1718047.0 - r12 * 769551.0 + r13 * 40025.0)
                    + r25 * (290999.0 + 2.0 * (- r1 * 215674.0 + 51507.0 * r12))
                    + r26 * 2.0 * (- 14213.0 + 9245.0 * r1)
                    + r27 * 121.0);
                const double ca4mu = -210.0 + r1 * 3381.0 - r12 * 5670.0 + r13 * 3220.0 - r14 * 645.0 + r15 * 30.0
                    + r2 * (-2331.0 - r1 * 2184.0 + r12 * 7350.0 - r13 * 3860.0 + r14 * 495.0)
                    + r22 * (5754.0 - r1 * 4074.0 - r12 * 1560.0 + r13 * 940.0)
                    + r23 * (-4396.0 + r1 * 3756.0 - r12 * 420.0)
                    + r24 * (1259.0 - r1 * 729.0)
                    + r25 * (-106.0);
                const double ca4r2 = 210.0 + r1 * 2121.0 - r12 * 6825.0 + r13 * 7000.0 - r14 * 2925.0 + r15 * 420.0 + r16 * 15.0
                    + r2 * (- 3591.0 + r1 * 3444.0 + r12 * 5565.0 - r13 * 7900.0 + r14 * 2475.0 - r15 * 90.0 - r16 * 45.0)
                    + r22 * (7791.0 - r1 * 14175.0 + r12 * 7020.0 - r13 * 1500.0 + r14 * 270.0 - r15 * 630.0)
                    + r23 * (-5740.0 + r1 * 10020.0 - r12 * 5520.0 + r13 * 1480.0 - r14 * 1090.0)
                    + r24 * (1135.0 - r1 * 555.0 + r12 * 180.0 + r13 * 570.0)
                    + r25 * (270.0 - r1 * 354.0 + r12 * 864.0)
                    + r26 * (-31.0 + 121.0 * r1);
                const double ca4r2m1 = 2.0 * ca4mu;

                return -1.0 / (20.0 * r2 * power_of<8>(r1 - r2)) * (power_of<4>(r1 - r2) * (C0 * ca00 + Cl * ca0mu * Lmu + Clr2 * ca0r2 * Lr2 + Cl * ca0r2m1 * Lr2m1)
                    + power_of<2>(r1 - r2) * (C0 * ca20 + Cl * ca2mu * Lmu + Clr2 * ca2r2 * Lr2 + Cl * ca2r2m1 * Lr2m1) * a2pi
                    + (C0 * ca40 + Cl * ca4mu * Lmu + Clr2 * ca4r2 * Lr2 + Cl * ca4r2m1 * Lr2m1) * a4pi);

            };
            auto T1Ttw2delta = [&] (const double & r1, const double & r2) -> double
            {
                static const double pi = M_PI, pi2 = pi * pi;

                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r12 * r12, r15 = r13 * r12, r16 = r13 * r13;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r22 * r22, r25 = r23 * r22, r26 = r23 * r23;
                const double L1mr1 = std::log(1.0 - r1), Lr2 = std::log(r2), Lr2m1 = std::log(r2 - 1.0), Lmu = std::log(mc2 / (mu * mu));
                const double L1mr1_ser = - 1. - r1 / 2. - r12 / 3. - r13 / 4.;
                const double dilogr1 = real(dilog(complex<double>(r1, 0.0)));
                const double dilog1mr2 = real(dilog(complex<double>(1.0 - r2, 0.0)));

                const double ca00 = r2 * (-14.0 + 6.0 * r1 + (6.0 + 2.0 * r1) * r2 + pi2 * (-1.0 + r1 + (1.0 - r1) * r2));
                const double ca0mu = r2 * (11.0 - 5.0 * r1 + (-5.0 - r1) * r2);
                const double ca01mr1 = 2.0 * (r1 - r12 + (1.0 - 4.0 * r1 + 3.0 * r12) * r2 + (-1.0 + 3.0 * r1 - 2.0 * r12) * r22);
                const double ca0r2m1 = 4.0 * (-1.0 + r1 + (2.0 - 2.0 * r1) * r2 + (-1.0 + 1.0 * r1) * r22);
                const double ca0log2 = 2.0 * r2 * (1.0 - r1 + (-1.0 + r1) * r2);
                const double ca0dlr1 = 2.0 * r2 * (1.0 - r1 + (-1.0 + r1) * r2);
                const double ca0dl1mr2 = 2.0 * r2 * (-3.0 + 3.0 * r1 + (3.0 - 3.0 * r1) * r2);

                const double ca20 = r2 * (10.0 * (pi2 + 30.0) - 20.0 * (pi2 + 22.0) * r1 + 12.0 * (pi2 + 14.0) * r12 - 2.0 * (pi2 + 6.0) * r13)
                    + r22 * (-20.0 * (pi2 + 22.0) + 36.0 * (pi2 + 14.0) * r1 - 18.0 * (pi2 + 6.0) * r12 + 2.0 * (pi2 - 2.0) * r13)
                    + r23 * (12.0 * (pi2 + 14.0) - 18.0 * (pi2 + 6.0) * r1 + 6.0 * (pi2 - 2.0) * r12)
                    + r24 * (-2.0 * (pi2 + 6.0) + 2.0 * (pi2 - 2.0) * r1);
                const double ca2mu = r2 * (-230.0 + 340.0 * r1 - 132.0 * r12 + 10.0 * r13)
                    + r22 * (340.0 - 396.0 * r1 + 90.0 * r12 + 2.0 * r13)
                    + r23 * (-132.0 + 90.0 * r1 + 6.0 * r12)
                    + r24 * (10.0 + 2.0 * r1);
                const double ca2l2 = r2 * (-10.0 + 20.0 * r1 - 12.0 * r12 + 2.0 * r13)
                    + r22 * (20.0 - 36.0 * r1 + 18.0 * r12 - 2.0 * r13)
                    + r23 * (-12.0 + 18.0 * r1 - 6.0 * r12)
                    + r24 * (2.0 - 2.0 * r1);
                const double ca2r2m1 = 40.0 - 80.0 * r1 + 48.0 * r12 - 8.0 * r13
                    + r2 * (-120.0 + 224.0 * r1 - 120.0 * r12 + 16.0 * r13)
                    + r22 * (128.0 - 216.0 * r1 + 96.0 * r12 - 8.0 * r13)
                    + r23 * (-56.0 + 80.0 * r1 - 24.0 * r12)
                    + r24 * (8.0 - 8.0 * r1);
                const double ca21mr1 = -20.0 * r1 + 40.0 * r12 - 24.0 * r13 + 4.0 * r14
                    + r2 * (-20.0 + 120.0 * r1 - 176.0 * r12 + 88.0 * r13 - 12.0 * r14)
                    + r22 * (40.0 - 176.0 * r1 + 216.0 * r12 - 88.0 * r13 + 8.0 * r14)
                    + r23 * (-24.0 + 88.0 * r1 - 88.0 * r12 + 24.0 * r13)
                    + r24 * (4.0 - 12.0 * r1 + 8.0 * r12);

                const double ca40 = r2 * (42.0 * (46.0 + pi2) - 126.0 * (38.0 + pi2) * r1 + 140.0 * (30.0 + pi2) * r12 - 70.0 * (22.0 + pi2) * r13 + 15.0 * (14.0 + pi2) * r14 - (6.0 + pi2) * r15)
                    + r22 * (-126.0 * (38.0 + pi2) + 350.0 * (30.0 + pi2) * r1 - 350.0 * (22.0 + pi2) * r12 + 150.0 * (14.0 + pi2) * r13 - 25.0 * (6.0 + pi2) * r14 + (-2.0 + pi2) * r15)
                    + r23 * (140.0 * (30.0 + pi2) - 350.0 * (22.0 + pi2) * r1 + 300.0 * (14.0 + pi2) * r12 - 100.0 * (6.0 + pi2) * r13 + 10.0 * (-2.0 + pi2) * r14)
                    + r24 * (-70.0 * (22.0 + pi2) + 150.0 * (14.0 + pi2) * r1 - 100.0 * (6.0 + pi2) * r12 + 20.0 * (-2.0 + pi2) * r13)
                    + r25 * (15.0 * (14.0 + pi2) - 25.0 * (6.0 + pi2) * r1 + 10.0 * (-2.0 + pi2) * r12)
                    + r26 * (-6.0 - pi2 + (-2.0 + pi2) * r1);
                const double ca4mu = r2 * (-1470.0 + 3654.0 * r1 - 3220.0 * r12 + 1190.0 * r13 - 165.0 * r14 + 5.0 * r15)
                    + r22 * (3654.0 - 8050.0 * r1 + 5950.0 * r12 - 1650.0 * r13 + 125.0 * r14 + r15)
                    + r23 * (-3220.0 + 5950.0 * r1 - 3300.0 * r12 + 500.0 * r13 + 10.0 * r14)
                    + r24 * (1190.0 - 1650.0 * r1 + 500.0 * r12 + 20.0 * r13)
                    + r25 * (-165.0 + 125.0 * r1 + 10.0 * r12)
                    + r26 * (5.0 + r1);
                const double ca4l2 = r2 * (-42.0 + 126.0 * r1 - 140.0 * r12 + 70.0 * r13 - 15.0 * r14 + r15)
                    + r22 * (126.0 - 350.0 * r1 + 350.0 * r12 - 150.0 * r13 + 25.0 * r14 - r15)
                    + r23 * (-140.0 + 350.0 * r1 - 300.0 * r12 + 100.0 * r13 - 10.0 * r14)
                    + r24 * (70.0 - 150.0 * r1 + 100.0 * r12 - 20.0 * r13)
                    + r25 * (-15.0 + 25.0 * r1 - 10.0 * r12)
                    + r26 * (1.0 - r1);
                const double ca4r2m1 = 168.0 - 504.0 * r1 + 560.0 * r12 - 280.0 * r13 + 60.0 * r14 - 4.0 * r15
                    + r2 * (-672.0 + 1904.0 * r1 - 1960.0 * r12 + 880.0 * r13 - 160.0 * r14 + 8.0 * r15)
                    + r22 * (1064.0 - 2800.0 * r1 + 2600.0 * r12 - 1000.0 * r13 + 140.0 * r14 - 4.0 * r15)
                    + r23 * (-840.0 + 2000.0 * r1 - 1600.0 * r12 + 480.0 * r13 - 40.0 * r14)
                    + r24 * (340.0 - 700.0 * r1 + 440.0 * r12 - 80.0 * r13)
                    + r25 * (-64.0 + 104.0 * r1 - 40.0 * r12)
                    + r26 * (4.0 - 4.0 * r1);
                const double ca41mr1 = -84.0 * r1 + 252.0 * r12 - 280.0 * r13 + 140.0 * r14 - 30.0 * r15 + 2.0 * r16
                    + r2 * (-84.0 + 672.0 * r1 - 1484.0 * r12 + 1400.0 * r13 - 610.0 * r14 + 112.0 * r15 - 6.0 * r16)
                    + r22 * (252.0 - 1484.0 * r1 + 2800.0 * r12 - 2300.0 * r13 + 850.0 * r14 - 122.0 * r15 + 4.0 * r16)
                    + r23 * (-280.0 + 1400.0 * r1 - 2300.0 * r12 + 1600.0 * r13 - 460.0 * r14 + 40.0 * r15)
                    + r24 * (140.0 - 610.0 * r1 + 850.0 * r12 - 460.0 * r13 + 80.0 * r14)
                    + r25 * (-30.0 + 112.0 * r1 - 122.0 * r12 + 40.0 * r13)
                    + r26 * (2.0 - 6.0 * r1 + 4.0 * r12);

                if ( std::abs(r1) < std::sqrt(std::numeric_limits<double>::epsilon()) ) {
                    return -3.0 / (r2 * power_of<7>(r1 - r2)) * (
                                power_of<4>(r1 - r2) * (ca00 + ca0mu * Lmu + ca01mr1 * L1mr1_ser + ca0r2m1 * Lr2m1
                                    + ca0log2 * (L1mr1_ser * (L1mr1_ser * r1 + Lr2 - 2.0 * Lr2m1) * r1 + Lr2m1 * (Lr2m1 - 2.0 * Lr2)) + ca0dlr1 * dilogr1 + ca0dl1mr2 * dilog1mr2)
                                - 3.0 * power_of<2>(r1 - r2) * (ca20 + ca2mu * Lmu + ca21mr1 * L1mr1_ser + ca2r2m1 * Lr2m1
                                    + ca2l2 * (2.0 * power_of<2>(L1mr1_ser * r1 - Lr2m1) - 4.0 * Lr2m1 * Lr2 + 2.0 * L1mr1_ser * Lr2 * r1 + 2.0 * dilogr1 - 6.0 * dilog1mr2)) * a2pi
                                - 15.0 * (ca40 + ca4mu * Lmu + ca4r2m1 * Lr2m1 + ca41mr1 * L1mr1_ser
                                    + ca4l2 * (2.0 * power_of<2>(L1mr1_ser * r1 - Lr2m1) - 4.0 * Lr2m1 * Lr2 + 2.0 * L1mr1_ser * Lr2 * r1 + 2.0 * dilogr1 - 6.0 * dilog1mr2)) * a4pi
                            );
                }

                return -3.0 / (r2 * power_of<7>(r1 - r2)) * (
                            power_of<4>(r1 - r2) * (ca00 + ca0mu * Lmu + ca01mr1 * L1mr1 / r1 + ca0r2m1 * Lr2m1
                                + ca0log2 * (L1mr1 * (L1mr1 + Lr2 - 2.0 * Lr2m1) + Lr2m1 * (Lr2m1 - 2.0 * Lr2)) + ca0dlr1 * dilogr1 + ca0dl1mr2 * dilog1mr2)
                            - 3.0 * power_of<2>(r1 - r2) * (ca20 + ca2mu * Lmu + ca21mr1 * L1mr1 / r1 + ca2r2m1 * Lr2m1
                                + ca2l2 * (2.0 * power_of<2>(L1mr1 - Lr2m1) - 4.0 * Lr2m1 * Lr2 + 2.0 * L1mr1 * Lr2 + 2.0 * dilogr1 - 6.0 * dilog1mr2)) * a2pi
                            - 15.0 * (ca40 + ca4mu * Lmu + ca4r2m1 * Lr2m1 + ca41mr1 * L1mr1 / r1
                                + ca4l2 * (2.0 * power_of<2>(L1mr1 - Lr2m1) - 4.0 * Lr2m1 * Lr2 + 2.0 * L1mr1 * Lr2 + 2.0 * dilogr1 - 6.0 * dilog1mr2)) * a4pi
                        );
            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    return 2.0 * (T1Ttw2thetarhom1(r1, r2) + T1Ttw2theta1mrho(r1, r2) + T1Ttw2delta(r1, r2))
                        * weight * std::exp(-mc2 * r2 / _M2);
                }
            );

            static const double eps = 1e-12;

            return mc * fpi * integrate<GSL::QAGS>(integrand, 1.0 + eps, s0TD(q2) / mc2, config);
        }

        double FT_nlo_tw3(const double & q2, const double _M2, const double & _select_weight = 0.0) const
        {
            // Reminder: q2 is the kinematic variable associated with the momentum
            // transfer, while s is the kinematic variable in which the function is
            // analytically continued. See also comment at beginning of Appendix B
            // of [DKMMO2008], p. 21.

            static const double pi2 = M_PI * M_PI;

            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double r1 = q2 / mc2;
            const double lmu = 2.0 * std::log(mc / mu());

            const double mupi = pi.mupi(mu);

            auto T1Ttw3ptheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double l = std::log((r2 - r1)/(r2 - 1.0));
                return l * (-1.0 + 6.0 * lr2m1 - 3.0 * lr2 + 3.0 * lmu);
            };
            auto T1Ttw3pthetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r13 * r1;
                const double r22 = r2 * r2, r23 = r22 * r2, r24 = r23 * r2;
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr1 = std::log(std::abs(r1)), l1mr1 = std::log(1.0 - r1);
                const double lr2mr1 = std::log(r2 - r1), l = std::log((r1 - r2)/(r1 - 1.0));
                const double dl = - 3.0 * (std::real(dilog(1.0 / r1)) + std::real(dilog(r2)) - std::real(dilog(r2 / r1)) + 2.0 * std::real(dilog((r2 - 1.0)/(r1 - 1.0))) + lr2 * (lr1 + lr2m1 - lr2mr1 - lr2 / 2.0));
                const double dl_ser = - 6.0 * std::real(dilog(1.0 - r2)) + 3.0 * std::real(dilog(1.0 / r2)) - pi2 + 3.0 * lr2 * (3.0 * lr2 / 2.0 - lr2m1)
                    + 3.0 * r1 * (r2 + (2.0 * r2 - 1.0) * lr2 - 1.0) / r2
                    + 3.0 * r12 * ((4.0 * r22 - 2.0) * lr2 + (r2 - 1.0) * (5.0 * r2 + 1.0)) / (4.0 * r22)
                    + r13 * ((6.0 * r23 - 3.0) * lr2 + (r2 - 1.0) * (2.0 * r2 * (5.0 * r2 + 2.0) + 1.0)) / (3.0 * r23)
                    + r14 * (12.0 * (2.0 * r24 - 1.0) * lr2 + (r2 - 1.0) * (r2 * (r2 * (47.0 * r2 + 23.0) + 11.0) + 3.0)) / (16.0 * r24);

                if ( std::abs(r1) < std::sqrt(std::numeric_limits<double>::epsilon()) )
                    return 3.0 * pi2 / 2.0 - 2.0 * lr2 + 3.0 * lmu * (l1mr1 - lr2mr1) + l * (1.0 - 6.0 * lr2m1) + dl_ser;
                return 3.0 * pi2 / 2.0 - 2.0 * lr2 + 3.0 * lmu * (l1mr1 - lr2mr1) + l * (1.0 - 6.0 * lr2m1) + dl;
            };

            auto T1Ttw3pdeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1;
                const double lr2 = std::log(r2);
                const double lr2m1 = std::log(r2 - 1.0);
                const double l1mr1 = std::log(1.0 - r1);
                const double l1mr1_ser = - 1.0 - r1 / 2.0 - r12 / 3.0 - r13 / 4.0;
                const double l = std::log((r2 - 1.0)/(1.0 - r1));
                const double dl = - std::real(dilog(r1)) - std::real(dilog(1.0 - r2));

                if ( std::abs(r1) < std::sqrt(std::numeric_limits<double>::epsilon()) )
                {
                    return (-5.0 * pi2 / 6.0 + (-1.0 + (4.0 + 1.0 / r2) * r1 - l1mr1_ser * r12) * l1mr1_ser + (-2.0 - 2.0 / r2 - 2.0 * l1mr1_ser * r1 + 3.0 * lr2m1) * lr2m1
                            + (l1mr1_ser * r1 - 2.0 * lr2m1) * lr2 + 2.0 * l * lmu + dl);
                }
                return (-5.0 * pi2 / 6.0 + (4.0 - 1.0 / r1 + 1.0 / r2 - l1mr1) * l1mr1 + (-2.0 - 2.0 / r2 - 2.0 * l1mr1 + 3.0 * lr2m1) * lr2m1
                        + (l1mr1 - 2.0 * lr2m1) * lr2 + 2.0 * l * lmu + dl);
            };
            auto T1Ttw3sigmatheta1mrho = [&] (const double & r1, const double & r2) -> double
            {
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr2mr1 = std::log(r2 - r1);

                return 3.0 * ((r1 - 1.0) * (- 4.0 + r2 * (3.0 - lr2 + lmu + 2.0 * lr2m1)) +
                        + (r1 - r2) * r2 * (lr2m1 * (1.0 + 3.0 * lr2 - 6.0 * lr2m1 + 6.0 * lr2mr1 - 3.0 * lmu)
                        + lr2mr1 * (- 1.0 - 3.0 * lr2 + 3.0 * lmu))
                        );
            };
            auto T1Ttw3sigmathetarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r14 = r13 * r1;
                const double r22 = r2 * r2, r23 = r22 * r2;
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double lr1 = std::log(std::abs(r1)), l1mr1 = std::log(1.0 - r1);
                const double lr2mr1 = std::log(r2 - r1);
                const double dl = r2 * (r1 - r2) * 3.0 * (std::real(dilog(1.0 / r1)) + std::real(dilog(r2)) - std::real(dilog(r2 / r1)) + 2.0 * std::real(dilog((r2 - 1.0)/(r1 - 1.0))) + lr2 * lr1);
                const double dl_ser = - r22 * (6.0 * std::real(dilog(1.0 - r2)) - 3.0 * std::real(dilog(1.0 / r2)) + pi2)
                    + r1 * r2 * (6.0 * std::real(dilog(1.0 - r2)) - 3.0 * std::real(dilog(1.0 / r2)) + 3.0 * r2 + 6.0 * r2 * lr2 + pi2 - 3.0)
                    + r12 * 3.0 * (3.0 - 8.0 * r2 + 5.0 * r2 + 4.0 * (r2 - 2.0) * r2 * lr2) / 4.0
                    + r13 * (5.0 / (4.0 * r2) + 6.0 - 69.0 * r2 / 4.0 + 10.0 * r22 + 3.0 * (2.0 * r2 - 3.0) * r2 * lr2) / 3.0
                    + r14 * ((r2 - 1.0) * (r2 * (r2 * (141.0 * r2 - 91.0) - 31.0) - 7.0) + 24.0 * (3.0 * r2 - 4.0) * r23 * lr2) / (48.0 * r22);

                if ( std::abs(r1) < std::sqrt(std::numeric_limits<double>::epsilon()) )
                {
                    return - 3.0 * (4.0 - 9.0 * r2 + 5.0 * r22
                        - lr2 * r2 * (- 3.0 + 2.0 * r2 - r1 * (2 * r2 - 3.0)) - 2.0 * lr2m1 * r2 * (r2 - 1.0) - lmu * r2 * (r2 - 1.0)
                        - r2 * (r1 - r2) * (6.0 * lr2 * (lr2mr1 - lr2m1 + lr2 / 2.0) + 12.0 * lr2m1 * (l1mr1 - lr2mr1)
                        + 2.0 * lr2mr1 * (1.0 - 3.0 * lmu) + 2.0 * l1mr1 * (-1.0 + 3.0 * lmu) + 3.0 * pi2) / 2.0
                        + dl_ser);
                }
                return - 3.0 * (4.0 - 9.0 * r2 + 5.0 * r22
                    - lr2 * r2 * (- 3.0 + 2.0 * r2 - r1 * (2 * r2 - 3.0)) - 2.0 * lr2m1 * r2 * (r2 - 1.0) - lmu * r2 * (r2 - 1.0)
                    - r2 * (r1 - r2) * (6.0 * lr2 * (lr2mr1 - lr2m1 + lr2 / 2.0) + 12.0 * lr2m1 * (l1mr1 - lr2mr1) 
                    + 2.0 * lr2mr1 * (1.0 - 3.0 * lmu) + 2.0 * l1mr1 * (-1.0 + 3.0 * lmu) + 3.0 * pi2) / 2.0
                    + dl);
            };
            auto T1Ttw3sigmadeltarhom1 = [&] (const double & r1, const double & r2) -> double
            {
                const double r12 = r1 * r1, r13 = r12 * r1, r22 = r2 * r2;
                const double l1mr1 = std::log(1.0 - r1);
                const double lr2 = std::log(r2), lr2m1 = std::log(r2 - 1.0);
                const double l = std::log((r2 - 1.0)/(1.0 - r1));

                const double l0 = r2 * (26.0 - 5.0 * r1 - 5.0 * r2 - (-12.0 + 11.0 * r1 + r2) * pi2 / 6.0);
                const double l1 = - (4.0 * r1 - 3.0 * r12 + (-6.0 * r1 + 2.0 * r12) * r2 + (1.0 + 2.0 * r1) * r22) * l1mr1 / r1;
                const double l1_ser = - (4.0 * r1 - 3.0 * r12 + (-6.0 * r1 + 2.0 * r12) * r2 + (1.0 + 2.0 * r1) * r22) * (-1.0 - r1 / 2.0 - r12 / 3.0 - r13 / 4.0);
                const double l2 = 2.0 * (4.0 - 3.0 * r1 + (-3.0 + r1) * r2 + r22) * lr2m1;
                const double l3 = r2 * (-14.0 + r1 + r2) * lmu;
                const double dl1 = r2 * ((-4.0 + r1 + 3.0 * r2) * l1mr1 * l1mr1 + (-4.0 + 5.0 * r1 - r2) * lr2m1 * lr2m1 + (-4.0 + 3.0 * r1 + r2) * l1mr1 * lr2
                        - 2.0 * (-4.0 + 3.0 * r1 + r2) * (l1mr1 + lr2) * lr2m1 + 2.0 * (r1 - r2) * l * lmu);
                const double dl2 = r2 * ((-4.0 + r1 + 3.0 * r2) * std::real(dilog(r1)) + (12.0 - 7.0 * r1 - 5.0 * r2) * std::real(dilog(1.0 - r2)));

                if ( std::abs(r1) < std::sqrt(std::numeric_limits<double>::epsilon()) )
                    return 3.0 * (l0 + l1_ser + l2 + l3 + dl1 + dl2);
                return 3.0 * (l0 + l1 + l2 + l3 + dl1 + dl2);
            };
            std::function<double (const double &)> integrand(
                [&] (const double & r2) -> double
                {
                    // _select_weight:
                    //  0.0 -> regular integral
                    //  1.0 -> integral of derivative w.r.t. -1/M^2
                    const double weight = (1.0 - _select_weight) + _select_weight * mc2 * r2;

                    return (
                            2.0 / power_of<2>(r2 - r1) * (T1Ttw3pthetarhom1(r1, r2) + T1Ttw3ptheta1mrho(r1, r2) + T1Ttw3pdeltarhom1(r1, r2))
                            + 2.0 / (3.0 * r2 * power_of<3>(r2 - r1)) * (T1Ttw3sigmatheta1mrho(r1, r2) + T1Ttw3sigmathetarhom1(r1, r2) + T1Ttw3sigmadeltarhom1(r1, r2)) 
                        ) * weight * std::exp(-mc2 * r2 / _M2);
                }
            );

            static const double eps = 1e-12;

            // _select_weight:
            //  0.0 -> regular integral
            //  1.0 -> integral of derivative w.r.t. -1/M^2
            const double weight = (1.0 - _select_weight) + _select_weight * mc2;

            return fpi * mupi * (integrate<GSL::QAGS>(integrand, 1.0 + eps, s0TD(q2) / mc2, config)
                    - 4.0 * (4.0 - 3.0 * lmu) * weight * std::exp(-mc2 / _M2) / power_of<2>(1.0 - q2 / mc2)
                    );
        }


        inline double _no_rescale_factor(const double &) const
        {
            return 1.0;
        }

        double _rescale_factor_p(const double & q2) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0_q2 = std::max(1e-10, (mc2 - q2) / (s0D(q2) - q2));
            const double u0_zero = std::max(1e-10, mc2 / s0D(q2));

            std::function<double (const double &)> integrand_numerator_q2(
                [&] (const double & u) -> double
                {
                    return u * (F_lo_tw2_integrand(u, q2, this->M2(), 0.0) + F_lo_tw3_integrand(u, q2, this->M2, 0.0));
                }
            );
            std::function<double (const double &)> integrand_denominator_q2(
                [&] (const double & u) -> double
                {
                    return (F_lo_tw2_integrand(u, q2, this->M2(), 0.0) + F_lo_tw3_integrand(u, q2, this->M2(), 0.0));
                }
            );
            std::function<double (const double &)> integrand_numerator_zero(
                [&] (const double & u) -> double
                {
                    return u * (F_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + F_lo_tw3_integrand(u, 0.0, this->M2(), 0.0));
                }
            );
            std::function<double (const double &)> integrand_denominator_zero(
                [&] (const double & u) -> double
                {
                    return (F_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + F_lo_tw3_integrand(u, 0.0, this->M2(), 0.0));
                }
            );

            double result = integrate<GSL::QAGS>(integrand_numerator_zero, u0_zero, 1.000, config) / integrate<GSL::QAGS>(integrand_numerator_q2, u0_q2, 1.000, config)
                / integrate<GSL::QAGS>(integrand_denominator_zero, u0_zero, 1.000, config) * integrate<GSL::QAGS>(integrand_denominator_q2, u0_q2, 1.000, config);
            return result;
        }

        double _rescale_factor_0(const double & q2) const
        {
            const double MD2 = MD * MD, mpi2 = mpi * mpi;
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0_q2 = std::max(1e-10, (mc2 - q2) / (s0tilD(q2) - q2));
            const double u0_zero = std::max(1e-10, mc2 / s0tilD(q2));

            std::function<double (const double &)> integrand_numerator_q2(
                [&] (const double & u) -> double
                {
                    const double F    = F_lo_tw2_integrand(u, q2, this->M2(), 0.0) + F_lo_tw3_integrand(u, q2, this->M2, 0.0);
                    const double Ftil = Ftil_lo_tw3_integrand(u, q2, this->M2, 0.0);
                    return u * (2.0 * q2 / (MD2 - mpi2) * Ftil + (1.0 - q2 / (MD2 - mpi)) * F);
                }
            );
            std::function<double (const double &)> integrand_denominator_q2(
                [&] (const double & u) -> double
                {
                    const double F    = F_lo_tw2_integrand(u, q2, this->M2(), 0.0) + F_lo_tw3_integrand(u, q2, this->M2, 0.0);
                    const double Ftil = Ftil_lo_tw3_integrand(u, q2, this->M2, 0.0);
                    return 2.0 * q2 / (MD2 - mpi2) * Ftil + (1.0 - q2 / (MD2 - mpi)) * F;
                }
            );
            std::function<double (const double &)> integrand_numerator_zero(
                [&] (const double & u) -> double
                {
                    const double F    = F_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + F_lo_tw3_integrand(u, 0.0, this->M2, 0.0);
                    return u * F;
                }
            );
            std::function<double (const double &)> integrand_denominator_zero(
                [&] (const double & u) -> double
                {
                    const double F    = F_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + F_lo_tw3_integrand(u, 0.0, this->M2, 0.0);
                    return F;
                }
            );

            double result = integrate<GSL::QAGS>(integrand_numerator_zero, u0_zero, 1.000, config) / integrate<GSL::QAGS>(integrand_numerator_q2, u0_q2, 1.000, config)
                / integrate<GSL::QAGS>(integrand_denominator_zero, u0_zero, 1.000, config) * integrate<GSL::QAGS>(integrand_denominator_q2, u0_q2, 1.000, config);

            return result;
        }

        double _rescale_factor_T(const double & q2) const
        {
            const double mc = this->m_c_msbar(mu), mc2 = mc * mc;
            const double u0_q2 = std::max(1e-10, (mc2 - q2) / (s0TD(q2) - q2));
            const double u0_zero = std::max(1e-10, mc2 / s0TD(q2));

            std::function<double (const double &)> integrand_numerator_q2(
                [&] (const double & u) -> double
                {
                    return u * (FT_lo_tw2_integrand(u, q2, this->M2(), 0.0) + FT_lo_tw3_integrand(u, q2, this->M2, 0.0));
                }
            );
            std::function<double (const double &)> integrand_denominator_q2(
                [&] (const double & u) -> double
                {
                    return (FT_lo_tw2_integrand(u, q2, this->M2(), 0.0) + FT_lo_tw3_integrand(u, q2, this->M2(), 0.0));
                }
            );
            std::function<double (const double &)> integrand_numerator_zero(
                [&] (const double & u) -> double
                {
                    return u * (FT_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + FT_lo_tw3_integrand(u, 0.0, this->M2(), 0.0));
                }
            );
            std::function<double (const double &)> integrand_denominator_zero(
                [&] (const double & u) -> double
                {
                    return (FT_lo_tw2_integrand(u, 0.0, this->M2(), 0.0) + FT_lo_tw3_integrand(u, 0.0, this->M2(), 0.0));
                }
            );

            double result = integrate<GSL::QAGS>(integrand_numerator_zero, u0_zero, 1.000, config) / integrate<GSL::QAGS>(integrand_numerator_q2, u0_q2, 1.000, config)
                / integrate<GSL::QAGS>(integrand_denominator_zero, u0_zero, 1.000, config) * integrate<GSL::QAGS>(integrand_denominator_q2, u0_q2, 1.000, config);

            return result;
        }

        double MDp_lcsr(const double & q2) const
        {
            const double M2_rescaled = this->M2() * this->rescale_factor_p(q2);
            const double alpha_s = model->alpha_s(mu);

            const double F_lo          = F_lo_tw2(q2, M2_rescaled, 0.0)  + F_lo_tw3(q2, M2_rescaled, 0.0)   + F_lo_tw4(q2, M2_rescaled, 0.0);
            const double F_lo_D1M2inv  = F_lo_tw2(q2, M2_rescaled, 1.0)  + F_lo_tw3(q2, M2_rescaled, 1.0)   + F_lo_tw4(q2, M2_rescaled, 1.0);
            const double F_nlo         = F_nlo_tw2(q2, M2_rescaled, 0.0) + F_nlo_tw3(q2, M2_rescaled, 0.0);
            const double F_nlo_D1M2inv = F_nlo_tw2(q2, M2_rescaled, 1.0) + F_nlo_tw3(q2, M2_rescaled, 1.0);

            const double F             = F_lo         + alpha_s / (3.0 * M_PI) * F_nlo;
            const double F_D1M2inv     = F_lo_D1M2inv + alpha_s / (3.0 * M_PI) * F_nlo_D1M2inv;

            double MD2 = F_D1M2inv / F;

            if (MD2 < 0.0)
                return 0.0;

            return std::sqrt(MD2);
        }

        double MD0_lcsr(const double & _q2) const 
        {
            const double _MD2 = MD * MD, mpi2 = mpi * mpi;
            const double q2 = (std::abs(_q2) > 1e-3) ? _q2 : 1e-3;

            const double M2_rescaled = this->M2() * this->rescale_factor_0(q2);
            const double alpha_s = model->alpha_s(mu);

            const double F_lo             = F_lo_tw2(q2, M2_rescaled, 0.0, 1.0)     + F_lo_tw3(q2, M2_rescaled, 0.0, 1.0)   + F_lo_tw4(q2, M2_rescaled, 0.0, 1.0);
            const double F_lo_D1M2inv     = F_lo_tw2(q2, M2_rescaled, 1.0, 1.0)     + F_lo_tw3(q2, M2_rescaled, 1.0, 1.0)   + F_lo_tw4(q2, M2_rescaled, 1.0, 1.0);
            const double F_nlo            = F_nlo_tw2(q2, M2_rescaled, 0.0) + F_nlo_tw3(q2, M2_rescaled, 0.0);
            const double F_nlo_D1M2inv    = F_nlo_tw2(q2, M2_rescaled, 1.0) + F_nlo_tw3(q2, M2_rescaled, 1.0);
            const double Ftil_lo          = Ftil_lo_tw3(q2, M2_rescaled, 0.0)       + Ftil_lo_tw4(q2, M2_rescaled, 0.0);
            const double Ftil_lo_D1M2inv  = Ftil_lo_tw3(q2, M2_rescaled, 1.0)       + Ftil_lo_tw4(q2, M2_rescaled, 1.0);
            const double Ftil_nlo         = Ftil_nlo_tw2(q2, M2_rescaled, 0.0)      + Ftil_nlo_tw3(q2, M2_rescaled, 0.0);
            const double Ftil_nlo_D1M2inv = Ftil_nlo_tw2(q2, M2_rescaled, 1.0)      + Ftil_nlo_tw3(q2, M2_rescaled, 1.0);

            const double F                = F_lo            + alpha_s / (3.0 * M_PI) * F_nlo;
            const double F_D1M2inv        = F_lo_D1M2inv    + alpha_s / (3.0 * M_PI) * F_nlo_D1M2inv;
            const double Ftil             = Ftil_lo         + alpha_s / (3.0 * M_PI) * Ftil_nlo;
            const double Ftil_D1M2inv     = Ftil_lo_D1M2inv + alpha_s / (3.0 * M_PI) * Ftil_nlo_D1M2inv;

            const double denom = 2.0 * q2 / (_MD2 - mpi2) * Ftil         + (1.0 - q2 / (_MD2 - mpi)) * F;
            const double num   = 2.0 * q2 / (_MD2 - mpi2) * Ftil_D1M2inv + (1.0 - q2 / (_MD2 - mpi)) * F_D1M2inv;
            double MD2 = num / denom;

            if (MD2 < 0.0)
            {
                return 0.0;
            }

            return std::sqrt(MD2);
        }

        double MDT_lcsr(const double & q2) const
        {
            const double M2_rescaled = this->M2() * this->rescale_factor_p(q2);
            const double alpha_s = model->alpha_s(mu);

            const double FT_lo          = FT_lo_tw2(q2, M2_rescaled, 0.0)  + FT_lo_tw3(q2, M2_rescaled, 0.0)   + FT_lo_tw4(q2, M2_rescaled, 0.0);
            const double FT_lo_D1M2inv  = FT_lo_tw2(q2, M2_rescaled, 1.0)  + FT_lo_tw3(q2, M2_rescaled, 1.0)   + FT_lo_tw4(q2, M2_rescaled, 1.0);
            const double FT_nlo         = FT_nlo_tw2(q2, M2_rescaled, 0.0) + FT_nlo_tw3(q2, M2_rescaled, 0.0);
            const double FT_nlo_D1M2inv = FT_nlo_tw2(q2, M2_rescaled, 1.0) + FT_nlo_tw3(q2, M2_rescaled, 1.0);

            const double FT             = FT_lo         + alpha_s / (3.0 * M_PI) * FT_nlo;
            const double FT_D1M2inv     = FT_lo_D1M2inv + alpha_s / (3.0 * M_PI) * FT_nlo_D1M2inv;

            double MD2 = FT_D1M2inv / FT;

            if (MD2 < 0.0)
                return 0.0;

            return std::sqrt(MD2);
        }

        double f_p(const double & q2) const
        {
            const double MD2 = MD * MD;
            const double M2_rescaled = this->M2() * this->rescale_factor_p(q2);
            const double fD = decay_constant();
            const double F_lo = F_lo_tw2(q2, M2_rescaled) + F_lo_tw3(q2, M2_rescaled) + F_lo_tw4(q2, M2_rescaled);
            const double F_nlo = F_nlo_tw2(q2, M2_rescaled) + F_nlo_tw3(q2, M2_rescaled);
            /*
             * we estimate the NNLO corrections to obey the relation |F_nnlo / F_nlo| = |F_nlo / F_lo|.
             * Therefore we set F_nnlo = F_nlo^2 / F_lo * zeta_nnlo, where zeta ranges between -1 and +1.
             */
            const double F_nnlo = F_nlo * F_nlo / F_lo * zeta_nnlo;
            const double alpha_s = model->alpha_s(mu);

            return std::exp(MD2 / M2_rescaled) / (2.0 * MD2 * fD) * (F_lo + alpha_s / (3.0 * M_PI) * F_nlo + alpha_s * alpha_s / (9.0 * M_PI * M_PI) * F_nnlo);
        }

        double f_0(const double & q2) const
        {

            if (std::abs(q2) < 1e-6)
                return f_p(q2);

            const double MD2 = MD * MD, mpi2 = mpi * mpi;
            const double M2_rescaled = this->M2() * this->rescale_factor_0(q2);
            const double fD = decay_constant();
            const double F_lo = F_lo_tw2(q2, M2_rescaled) + F_lo_tw3(q2, M2_rescaled) + F_lo_tw4(q2, M2_rescaled);
            const double F_nlo = F_nlo_tw2(q2, M2_rescaled) + F_nlo_tw3(q2, M2_rescaled);
            const double Ftil_lo = Ftil_lo_tw3(q2, M2_rescaled) + Ftil_lo_tw4(q2, M2_rescaled);
            const double Ftil_nlo = Ftil_nlo_tw2(q2, M2_rescaled) + Ftil_nlo_tw3(q2, M2_rescaled);
            //const double Ftil_nnlo = F_nlo * F_nlo / F_lo * zeta_nnlo;
            const double alpha_s = model->alpha_s(mu);

            return std::exp(MD2 / M2_rescaled) / (2.0 * MD2 * fD) * (2.0 * q2 / (MD2 - mpi2) * (Ftil_lo + alpha_s / (3.0 * M_PI) * Ftil_nlo) + (1.0 - q2 / (MD2 - mpi)) * (F_lo + alpha_s / (3.0 * M_PI) * F_nlo));
        }

        double f_t(const double & q2) const
        {
            const double MD2 = MD * MD;
            const double M2_rescaled = this->M2() * this->rescale_factor_T(q2);
            const double fD = decay_constant();
            const double FT_lo = FT_lo_tw2(q2, M2_rescaled) + FT_lo_tw3(q2, M2_rescaled) + FT_lo_tw4(q2, M2_rescaled);
            const double FT_nlo = FT_nlo_tw2(q2, M2_rescaled) + FT_nlo_tw3(q2, M2_rescaled);
            //const double FT_nnlo = FT_nlo * FT_nlo / FT_lo * zeta_nnlo;
            const double alpha_s = model->alpha_s(mu);

            return std::exp(MD2 / M2_rescaled) / (2.0 * MD2 * fD) * (MD + mpi) * (FT_lo + alpha_s / (3.0 * M_PI) * FT_nlo);
        }

        Diagnostics diagnostics() const
        {
            Diagnostics results;

            // Function rho_1, cf. [KKMO:2009A], eq. (C.2)
            {
                results.add(Diagnostics::Entry{ rho_1(6.5, 1.27, 1.4), "rho_1(s = 6.5, m_c = 1.27, mu = 1.4), [KKMO:2009A]" });
                results.add(Diagnostics::Entry{ rho_1(7.0, 1.27, 1.4), "rho_1(s = 7.0, m_c = 1.27, mu = 1.4), [KKMO:2009A]" });
                results.add(Diagnostics::Entry{ rho_1(7.5, 1.27, 1.4), "rho_1(s = 7.5, m_c = 1.27, mu = 1.4), [KKMO:2009A]" });
            }

            results.add(Diagnostics::Entry{ this->decay_constant(), "f_D, [KKMO:2009A]" });

            results.add(Diagnostics::Entry{ this->rescale_factor_p( 0.0), "rescale_factor_p(s =  0.0), [KKMO:2009A]" });
            results.add(Diagnostics::Entry{ this->rescale_factor_p(10.0), "rescale_factor_p(s = 10.0), [KKMO:2009A]" });

            results.add(Diagnostics::Entry{ this->rescale_factor_0( 0.0), "rescale_factor_0(s =  0.0), [KKMO:2009A]" });
            results.add(Diagnostics::Entry{ this->rescale_factor_0(10.0), "rescale_factor_0(s = 10.0), [KKMO:2009A]" });

            results.add(Diagnostics::Entry{ this->rescale_factor_T( 0.0), "rescale_factor_T(s =  0.0), [KKMO:2009A]" });
            results.add(Diagnostics::Entry{ this->rescale_factor_T(10.0), "rescale_factor_T(s = 10.0), [KKMO:2009A]" });

            results.add(Diagnostics::Entry{ this->MDp_lcsr( 0.0), "M_D(f_+, q2 =  0.0), [KKMO:2009A]"});
            results.add(Diagnostics::Entry{ this->MDp_lcsr(10.0), "M_D(f_+, q2 =  0.0), [KKMO:2009A]"});

            results.add(Diagnostics::Entry{ this->MD0_lcsr( 0.0), "M_D(f_0, q2 =  0.0), [KKMO:2009A]"});
            results.add(Diagnostics::Entry{ this->MD0_lcsr(10.0), "M_D(f_0, q2 = 10.0), [KKMO:2009A]"});

            results.add(Diagnostics::Entry{ this->MDT_lcsr( 0.0), "M_D(f_T, q2 =  0.0), [KKMO:2009A]"});
            results.add(Diagnostics::Entry{ this->MDT_lcsr(10.0), "M_D(f_T, q2 = 10.0), [KKMO:2009A]"});

            return results;
        }
    };

    const std::vector<OptionSpecification>
    Implementation<AnalyticFormFactorDToPiKKMO2009>::options
    {
        { "rescale-borel", { "1", "0" }, "1" }
    };

    AnalyticFormFactorDToPiKKMO2009::AnalyticFormFactorDToPiKKMO2009(const Parameters & p, const Options & o) :
        PrivateImplementationPattern<AnalyticFormFactorDToPiKKMO2009>(new Implementation<AnalyticFormFactorDToPiKKMO2009>(p, o, *this))
    {
    }

    AnalyticFormFactorDToPiKKMO2009::~AnalyticFormFactorDToPiKKMO2009()
    {
    }

    FormFactors<PToP> *
    AnalyticFormFactorDToPiKKMO2009::make(const Parameters & p, const Options & o)
    {
        return new AnalyticFormFactorDToPiKKMO2009(p, o);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::F_lo_tw2(const double & q2) const
    {
        return _imp->F_lo_tw2(q2, _imp->M2() * _imp->rescale_factor_p(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::F_lo_tw3(const double & q2) const
    {
        return _imp->F_lo_tw3(q2, _imp->M2() * _imp->rescale_factor_p(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::F_lo_tw4(const double & q2) const
    {
        return _imp->F_lo_tw4(q2, _imp->M2() * _imp->rescale_factor_p(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::F_nlo_tw2(const double & q2) const
    {
        return _imp->F_nlo_tw2(q2, _imp->M2() * _imp->rescale_factor_p(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::F_nlo_tw3(const double & q2) const
    {
        return _imp->F_nlo_tw3(q2, _imp->M2() * _imp->rescale_factor_p(q2));
    }
    
    double
    AnalyticFormFactorDToPiKKMO2009::Ftil_lo_tw3(const double & q2) const
    {
        return _imp->Ftil_lo_tw3(q2, _imp->M2() * _imp->rescale_factor_0(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::Ftil_lo_tw4(const double & q2) const
    {
        return _imp->Ftil_lo_tw4(q2, _imp->M2() * _imp->rescale_factor_0(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::Ftil_nlo_tw2(const double & q2) const
    {
        return _imp->Ftil_nlo_tw2(q2, _imp->M2() * _imp->rescale_factor_0(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::Ftil_nlo_tw3(const double & q2) const
    {
        return _imp->Ftil_nlo_tw3(q2, _imp->M2() * _imp->rescale_factor_0(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::FT_lo_tw2(const double & q2) const
    {
        return _imp->FT_lo_tw2(q2, _imp->M2() * _imp->rescale_factor_T(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::FT_lo_tw3(const double & q2) const
    {
        return _imp->FT_lo_tw3(q2, _imp->M2() * _imp->rescale_factor_T(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::FT_lo_tw4(const double & q2) const
    {
        return _imp->FT_lo_tw4(q2, _imp->M2() * _imp->rescale_factor_T(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::FT_nlo_tw2(const double & q2) const
    {
        return _imp->FT_nlo_tw2(q2, _imp->M2() * _imp->rescale_factor_T(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::FT_nlo_tw3(const double & q2) const
    {
        return _imp->FT_nlo_tw3(q2, _imp->M2() * _imp->rescale_factor_T(q2));
    }

    double
    AnalyticFormFactorDToPiKKMO2009::f_p(const double & q2) const
    {
        return _imp->f_p(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::f_0(const double & q2) const
    {
        return _imp->f_0(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::f_t(const double & q2) const
    {
        return _imp->f_t(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::f_plus_T(const double &) const
    {
        return 0.0;
    }

    double
    AnalyticFormFactorDToPiKKMO2009::MDp_lcsr(const double & q2) const
    {
        return _imp->MDp_lcsr(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::MD0_lcsr(const double & q2) const
    {
        return _imp->MD0_lcsr(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::MDT_lcsr(const double & q2) const
    {
        return _imp->MDT_lcsr(q2);
    }

    double
    AnalyticFormFactorDToPiKKMO2009::MD_svz() const
    {
        return _imp->MD_svz();
    }

    double
    AnalyticFormFactorDToPiKKMO2009::decay_constant() const
    {
        return _imp->decay_constant();
    }

    Diagnostics
    AnalyticFormFactorDToPiKKMO2009::diagnostics() const
    {
        return _imp->diagnostics();
    }

    const std::set<ReferenceName>
    AnalyticFormFactorDToPiKKMO2009::references
    {
        "KKMO:2009A"_rn,
        "LMvD:2021A"_rn
    };

    std::vector<OptionSpecification>::const_iterator
    AnalyticFormFactorDToPiKKMO2009::begin_options()
    {
        return Implementation<AnalyticFormFactorDToPiKKMO2009>::options.cbegin();
    }

    std::vector<OptionSpecification>::const_iterator
    AnalyticFormFactorDToPiKKMO2009::end_options()
    {
        return Implementation<AnalyticFormFactorDToPiKKMO2009>::options.cend();
    }
}
