

#include <deal.II/grid/grid_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/base/work_stream.h>

#include "PoissonSolver.h"
#include "Globals.h"


namespace femocs {

// ----------------------------------------------------------------------------------------
/* Class for outputting the field distribution
 * being calculated from potential distribution */
template <int dim>
class LaplacePostProcessor : public DataPostprocessorVector<dim> {
public:
    LaplacePostProcessor() : DataPostprocessorVector<dim>("field", update_gradients) {}

    void
    compute_derived_quantities_scalar (
            const vector<double>             &/*uh*/,
            const vector<Tensor<1,dim> >     &duh,
            const vector<Tensor<2,dim> >     &/*dduh*/,
            const vector<Point<dim> >        &/*normals*/,
            const vector<Point<dim> >        &/*evaluation_points*/,
            vector<Vector<double> >          &computed_quantities) const {
        for (unsigned int i=0; i<computed_quantities.size(); i++) {
            for (unsigned int d=0; d<dim; ++d)
                computed_quantities[i](d) = duh[i][d];

        }
    }
};
// ----------------------------------------------------------------------------

template<int dim>
PoissonSolver<dim>::PoissonSolver() : DealSolver<dim>(),
        conf(NULL), interpolator(NULL), applied_field(0), applied_potential(0)
        {}

template<int dim>
PoissonSolver<dim>::PoissonSolver(const Config::Field* conf_, const LinearHexahedra* interpolator_) :
        DealSolver<dim>(),
        conf(conf_), interpolator(interpolator_),
        applied_field(0), applied_potential(0)
        {}

template<int dim>
void PoissonSolver<dim>::mark_mesh() {
    this->mark_boundary(BoundaryID::vacuum_top, BoundaryID::copper_surface,
            BoundaryID::vacuum_sides, BoundaryID::copper_surface);
}

template<int dim>
double PoissonSolver<dim>::probe_efield_norm(const Point<dim> &p) const {
    return VectorTools::point_gradient (this->dof_handler, this->solution, p).norm();
}

template<int dim>
double PoissonSolver<dim>::probe_potential(const Point<dim> &p) const{
    return VectorTools::point_value(this->dof_handler, this->solution, p);
}

template<int dim>
double PoissonSolver<dim>::probe_potential(const Point<dim> &p, int cell_index) const {
    return probe_potential(p, cell_index, StaticMappingQ1<dim,dim>::mapping);
}

template<int dim>
double PoissonSolver<dim>::probe_potential(const Point<dim> &p, const int cell_index, Mapping<dim,dim>& mapping) const {

    //get active cell iterator from cell index
    typename DoFHandler<dim>::active_cell_iterator cell(&this->triangulation, 0, max(0,cell_index), &this->dof_handler);

    //point in transformed unit cell coordinates
    Point<dim> p_cell;

    if (cell_index < 0){ // in case the cell index is unknown (argument cell_index < 0)
        const pair<typename DoFHandler<dim,dim>::active_cell_iterator, Point<dim> > cell_point
        = GridTools::find_active_cell_around_point (mapping, this->dof_handler, p);
        cell = cell_point.first;
        p_cell = cell_point.second;
    }else // cell index is known
        p_cell = mapping.transform_real_to_unit_cell(cell, p);


    //create virtual quadrature point
    const Quadrature<dim> quadrature(GeometryInfo<dim>::project_to_unit_cell(p_cell));

    FEValues<dim> fe_values(mapping, this->fe, quadrature, update_values);
    fe_values.reinit(cell);

    vector<Vector<double> > u_value(1, Vector<double> (this->fe.n_components()));
    fe_values.get_function_values(this->solution, u_value);

    return u_value[0][0];
}

template<int dim>
double PoissonSolver<dim>::probe_efield_norm(const Point<dim> &p, int cell_index) const {
    return probe_efield(p, cell_index, StaticMappingQ1<dim,dim>::mapping).norm();
}

template<int dim>
Tensor<1, dim, double> PoissonSolver<dim>::probe_efield(const Point<dim> &p, int cell_index) const {
    return probe_efield(p, cell_index, StaticMappingQ1<dim,dim>::mapping);
}

template<int dim>
Tensor<1, dim, double> PoissonSolver<dim>::probe_efield(const Point<dim> &p, const int cell_index, Mapping<dim,dim>& mapping) const {

    //get active cell iterator from cell index
    typename DoFHandler<dim>::active_cell_iterator cell(&this->triangulation, 0, max(0,cell_index), &this->dof_handler);

    // transform the point from real to unit cell coordinates
    Point<dim> p_cell;
    if (cell_index < 0){
        const pair<typename DoFHandler<dim,dim>::active_cell_iterator, Point<dim> > cell_point
        = GridTools::find_active_cell_around_point (mapping, this->dof_handler, p);
        cell = cell_point.first;
        p_cell = cell_point.second;
    }else
        p_cell = mapping.transform_real_to_unit_cell(cell, p);

    const Quadrature<dim> quadrature(GeometryInfo<dim>::project_to_unit_cell(p_cell));

    FEValues<dim> fe_values(mapping, this->fe, quadrature, update_gradients);
    fe_values.reinit(cell);

    vector<vector<Tensor<1, dim, double> > >
    u_gradient(1, vector<Tensor<1, dim, double> > (this->fe.n_components()));
    fe_values.get_function_gradients(this->solution, u_gradient);

    return -u_gradient[0][0];
}

template<int dim>
void PoissonSolver<dim>::export_charge_dens(vector<double> &charge_dens) const {
    const int n_verts = this->tria->n_used_vertices();
    require(n_verts == this->vertex2dof.size(), "Mismatch between #vertices and vertex2dof size: "
            + d2s(n_verts) + " vs " + d2s(this->vertex2dof.size()));

    charge_dens.resize(n_verts);
    for (unsigned i = 0; i < n_verts; i++)
        charge_dens[i] = charge_density[this->vertex2dof[i]];
}

template<int dim>
double PoissonSolver<dim>::get_face_bc(const unsigned int face) const {
    return applied_field;
}

template<int dim>
void PoissonSolver<dim>::setup(const double field, const double potential) {
    DealSolver<dim>::setup_system();
    applied_field = field;
    applied_potential = potential;
}

template<int dim>
void PoissonSolver<dim>::assemble(const bool full_run) {
    require(conf, "NULL conf can't be used!");
    require(conf->anode_BC == "neumann" || conf->anode_BC == "dirichlet",
            "Unimplemented anode BC: " + conf->anode_BC);

    if (full_run) this->system_matrix = 0;
    this->system_rhs = 0;

    if (conf->anode_BC == "neumann") {
        if (full_run) {
            assemble_parallel();
            this->append_dirichlet(BoundaryID::copper_surface, this->dirichlet_bc_value);
        }
        this->assemble_rhs(BoundaryID::vacuum_top);
    } else {
        if (full_run) {
            assemble_parallel();
            this->append_dirichlet(BoundaryID::copper_surface, this->dirichlet_bc_value);
            this->append_dirichlet(BoundaryID::vacuum_top, applied_potential);
        }
    }

    // save charge density for writing it to file
    // must be before applying Diriclet BCs
    if (this->write_time()) {
        int n_verts = this->tria->n_used_vertices();
        this->charge_density = this->system_rhs;
        this->calc_dof_volumes();
        for (unsigned i = 0; i < n_verts; i++) {
            int dof = this->vertex2dof[i];
            this->charge_density[dof] /= this->dof_volume[dof];
        }
    }
    if (full_run) this->apply_dirichlet();
}

template<int dim>
void PoissonSolver<dim>::assemble_parallel() {
    LinearSystem system(&this->system_rhs, &this->system_matrix);
    QGauss<dim> quadrature_formula(this->quadrature_degree);

    const unsigned int n_dofs = this->fe.dofs_per_cell;
    const unsigned int n_q_points = quadrature_formula.size();

    WorkStream::run(this->dof_handler.begin_active(),this->dof_handler.end(),
            std_cxx11::bind(&PoissonSolver<dim>::assemble_local_cell,
                    this,
                    std_cxx11::_1,
                    std_cxx11::_2,
                    std_cxx11::_3),
            std_cxx11::bind(&PoissonSolver<dim>::copy_global_cell,
                    this,
                    std_cxx11::_1,
                    std_cxx11::ref(system)),
            ScratchData(this->fe, quadrature_formula, update_gradients | update_quadrature_points | update_JxW_values),
            CopyData(n_dofs, n_q_points)
    );
}

template<int dim>
void PoissonSolver<dim>::assemble_local_cell(const typename DoFHandler<dim>::active_cell_iterator &cell,
        ScratchData &scratch_data, CopyData &copy_data) const
{
    const unsigned int n_dofs = copy_data.n_dofs;
    const unsigned int n_q_points = copy_data.n_q_points;

    scratch_data.fe_values.reinit(cell);

    // Local matrix assembly
    copy_data.cell_matrix = 0;
    for (unsigned int q = 0; q < n_q_points; ++q) {
        for (unsigned int i = 0; i < n_dofs; ++i) {
            for (unsigned int j = 0; j < n_dofs; ++j) {
                copy_data.cell_matrix(i, j) += scratch_data.fe_values.JxW(q) *
                scratch_data.fe_values.shape_grad(i, q) * scratch_data.fe_values.shape_grad(j, q);
            }
        }
    }

    // Nothing to add to local right-hand-side vector assembly
//    copy_data.cell_rhs = 0;

    // Obtain dof indices for updating global matrix and right-hand-side vector
    cell->get_dof_indices(copy_data.dof_indices);
}

template<int dim>
void PoissonSolver<dim>::write_vtk(ofstream& out) const {
    LaplacePostProcessor<dim> post_processor; // needs to be before data_out
    DataOut<dim> data_out;

    data_out.attach_dof_handler(this->dof_handler);
    data_out.add_data_vector(this->solution, "electric_potential");
    data_out.add_data_vector(this->solution, post_processor);

    data_out.build_patches();
    data_out.write_vtk(out);
}

template class PoissonSolver<3> ;

} // namespace femocs
