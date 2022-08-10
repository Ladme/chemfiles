// Chemfiles, a modern library for chemistry file reading and writing
// Copyright (C) Guillaume Fraux and contributors -- BSD license

#include <cassert>
#include <cstdint>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "chemfiles/error_fmt.hpp"
#include "chemfiles/external/optional.hpp"
#include "chemfiles/external/span.hpp"
#include "chemfiles/periodic_table.hpp"
#include "chemfiles/types.hpp"
#include "chemfiles/warnings.hpp"

#include "chemfiles/Atom.hpp"
#include "chemfiles/File.hpp"
#include "chemfiles/FormatMetadata.hpp"
#include "chemfiles/Frame.hpp"
#include "chemfiles/Residue.hpp"

#include "chemfiles/files/XDRFile.hpp"
#include "chemfiles/formats/TPR.hpp"

// see <GMX>/src/gromacs/fileio/tpxio.cpp
#define TPR_TAG_RELEASE "release"

// GROMACS explains: Enum of values that describe the contents of a TPR file
// whose format matches a version number.
// see `tpxv` in <GMX>/src/gromacs/fileio/tpxio.cpp
enum TPRVersion {
    tprv_ComputationalElectrophysiology = 96,
    tprv_Use64BitRandomSeed,
    tprv_RestrictedBendingAndCombinedAngleTorsionPotentials,
    tprv_InteractiveMolecularDynamics,
    tprv_RemoveObsoleteParameters1,
    tprv_PullCoordTypeGeom,
    tprv_PullGeomDirRel,
    tprv_IntermolecularBondeds,
    tprv_CompElWithSwapLayerOffset,
    tprv_CompElPolyatomicIonsAndMultipleIonTypes,
    tprv_RemoveAdress,
    tprv_PullCoordNGroup,
    tprv_RemoveTwinRange,
    tprv_ReplacePullPrintCOM12,
    tprv_PullExternalPotential,
    tprv_GenericParamsForElectricField,
    tprv_AcceleratedWeightHistogram,
    tprv_RemoveImplicitSolvation,
    tprv_PullPrevStepCOMAsReference,
    tprv_MimicQMMM,
    tprv_PullAverage,
    tprv_GenericInternalParameters,
    tprv_VSite2FD,
    tprv_AddSizeField,
    tprv_StoreNonBondedInteractionExclusionGroup,
    tprv_VSite1,
    tprv_MTS,
    tprv_RemovedConstantAcceleration,
    tprv_TransformationPullCoord,
    tprv_SoftcoreGapsys,
    tprv_ReaddedConstantAcceleration,
    tprv_RemoveTholeRfac,
    tprv_RemoveAtomtypes,
    tprv_Count // This number is for the total number of versions
};

// GROMACS explains:
// Version number of the file format written to run input
// files by this version of the code.
//
// The TPR version increases whenever the file format in the main
// development branch changes, due to an extension of the `TPRVersion` enum above.
// Backward compatibility for reading old run input files is maintained
// by checking this version number against that of the file and then using
// the correct code path.
static const int TPR_VERSION = tprv_Count - 1;

// GROMACS explains:
// Value of current TPR generation to keep track of incompatible changes for older TPR versions.
//
// The generation should be incremented when editing the TOPOLOGY or HEADER of the TPR format.
// In particular, updating `FUNCTION_TYPE_UPDATES` or changing the fields of the TPR header
// often triggers such needs.
//
// This way GROMACS can maintain forward compatibility for all analysis tools and/or external
// programs that only need to know the atom/residue names, charges, and bond connectivity.
//
// For chemfiles, this means it is possible to read TPR files from future versions as long as they
// are the same generation as specified here.
// The generation is tracked by GROMACS with an enum.
// see `TpxGeneration` in in <GMX>/src/gromacs/fileio/tpxio.cpp
static const int TPR_GENERATION = 28;

// This number should be the most recent backwards incompatible version.
// I.e., if this number is 9, we cannot read TPR version 9 with this code.
static const int TPR_INCOMPATIBLE_VERSION = 57; // GMX4.0 has version 58

// see <GMX>/api/legacy/include/gromacs/topology/ifunc.h
enum FunctionType {
    F_BONDS,
    F_G96BONDS,
    F_MORSE,
    F_CUBICBONDS,
    F_CONNBONDS,
    F_HARMONIC,
    F_FENEBONDS,
    F_TABBONDS,
    F_TABBONDSNC,
    F_RESTRBONDS,
    F_ANGLES,
    F_G96ANGLES,
    F_RESTRANGLES,
    F_LINEAR_ANGLES,
    F_CROSS_BOND_BONDS,
    F_CROSS_BOND_ANGLES,
    F_UREY_BRADLEY,
    F_QUARTIC_ANGLES,
    F_TABANGLES,
    F_PDIHS,
    F_RBDIHS,
    F_RESTRDIHS,
    F_CBTDIHS,
    F_FOURDIHS,
    F_IDIHS,
    F_PIDIHS,
    F_TABDIHS,
    F_CMAP,
    F_GB12_NOLONGERUSED,
    F_GB13_NOLONGERUSED,
    F_GB14_NOLONGERUSED,
    F_GBPOL_NOLONGERUSED,
    F_NPSOLVATION_NOLONGERUSED,
    F_LJ14,
    F_COUL14,
    F_LJC14_Q,
    F_LJC_PAIRS_NB,
    F_LJ,
    F_BHAM,
    F_LJ_LR_NOLONGERUSED,
    F_BHAM_LR_NOLONGERUSED,
    F_DISPCORR,
    F_COUL_SR,
    F_COUL_LR_NOLONGERUSED,
    F_RF_EXCL,
    F_COUL_RECIP,
    F_LJ_RECIP,
    F_DPD,
    F_POLARIZATION,
    F_WATER_POL,
    F_THOLE_POL,
    F_ANHARM_POL,
    F_POSRES,
    F_FBPOSRES,
    F_DISRES,
    F_DISRESVIOL,
    F_ORIRES,
    F_ORIRESDEV,
    F_ANGRES,
    F_ANGRESZ,
    F_DIHRES,
    F_DIHRESVIOL,
    F_CONSTR,
    F_CONSTRNC,
    F_SETTLE,
    F_VSITE1,
    F_VSITE2,
    F_VSITE2FD,
    F_VSITE3,
    F_VSITE3FD,
    F_VSITE3FAD,
    F_VSITE3OUT,
    F_VSITE4FD,
    F_VSITE4FDN,
    F_VSITEN,
    F_COM_PULL,
    F_DENSITYFITTING,
    F_EQM,
    F_EPOT,
    F_EKIN,
    F_ETOT,
    F_ECONSERVED,
    F_TEMP,
    F_VTEMP_NOLONGERUSED,
    F_PDISPCORR,
    F_PRES,
    F_DVDL_CONSTR,
    F_DVDL,
    F_DKDL,
    F_DVDL_COUL,
    F_DVDL_VDW,
    F_DVDL_BONDED,
    F_DVDL_RESTRAINT,
    F_DVDL_TEMPERATURE,
    F_NRE // This number is for the total number of energies
};

// All force filed parameters of the system.
// see `gmx_ffparams_t` in <GMX>/api/legacy/include/gromacs/topology/forcefieldparameters.h
struct FFParams {
    // Number of non-bonded atom types
    int natom_types;
    // Function type of an interaction
    std::vector<int32_t> function_types;
};

// Maximum allowed number of parameters.
// see <GMX>/api/legacy/include/gromacs/topology/ifunc.h
const int NR_RBDIHS = 6;
const int NR_CBTDIHS = 6;

// see `t_ftupd` in <GMX>/src/gromacs/fileio/tpxio.cpp
struct FunctionTypeUpdate {
    // File version number in which the function type first appeared
    int file_version;
    FunctionType function_type;
};

// The following Table is used maintain TRR compatibility  when function types
// are added. Necessary to support reading of old TPR file versions.
// see `ftupd` in <GMX>/src/gromacs/fileio/tpxio.cpp
static const FunctionTypeUpdate FUNCTION_TYPE_UPDATES[24]{
    {70, F_RESTRBONDS},
    {tprv_RestrictedBendingAndCombinedAngleTorsionPotentials, F_RESTRANGLES},
    {76, F_LINEAR_ANGLES},
    {tprv_RestrictedBendingAndCombinedAngleTorsionPotentials, F_RESTRDIHS},
    {tprv_RestrictedBendingAndCombinedAngleTorsionPotentials, F_CBTDIHS},
    {65, F_CMAP},
    {60, F_GB12_NOLONGERUSED},
    {61, F_GB13_NOLONGERUSED},
    {61, F_GB14_NOLONGERUSED},
    {72, F_GBPOL_NOLONGERUSED},
    {72, F_NPSOLVATION_NOLONGERUSED},
    {93, F_LJ_RECIP},
    {76, F_ANHARM_POL},
    {90, F_FBPOSRES},
    {tprv_VSite1, F_VSITE1},
    {tprv_VSite2FD, F_VSITE2FD},
    {tprv_GenericInternalParameters, F_DENSITYFITTING},
    {69, F_VTEMP_NOLONGERUSED},
    {66, F_PDISPCORR},
    {79, F_DVDL_COUL},
    {79, F_DVDL_VDW},
    {79, F_DVDL_BONDED},
    {79, F_DVDL_RESTRAINT},
    {79, F_DVDL_TEMPERATURE},
};

// see `t_interaction_function` in <GMX>/api/legacy/include/gromacs/topology/ifunc.h
struct FunctionTypeInfo {
    std::string name;
    size_t natoms;
};

// This table contains a human-readable name for a function type
// and the number of atoms needed per function.
// see `interaction_function` in <GMX>/src/gromacs/topology/ifunc.cpp
const FunctionTypeInfo FUNCTION_TYPE_INFOS[F_NRE]{
    {"Bond", 2},
    {"G96Bond", 2},
    {"Morse", 2},
    {"Cubic Bonds", 2},
    {"Connect Bonds", 2},
    {"Harmonic Pot.", 2},
    {"FENE Bonds", 2},
    {"Tab. Bonds", 2},
    {"Tab. Bonds NC", 2},
    {"Restraint Pot.", 2},
    {"Angle", 3},
    {"G96Angle", 3},
    {"Restricted Angles", 3},
    {"Lin. Angle", 3},
    {"Bond-Cross", 3},
    {"BA-Cross", 3},
    {"U-B", 3},
    {"Quartic Angles", 3},
    {"Tab. Angles", 3},
    {"Proper Dih.", 4},
    {"Ryckaert-Bell.", 4},
    {"Restricted Dih.", 4},
    {"CBT Dih.", 4},
    {"Fourier Dih.", 4},
    {"Improper Dih.", 4},
    {"Periodic Improper Dih.", 4},
    {"Tab. Dih.", 4},
    {"CMAP Dih.", 5},
    {"GB 1-2 Pol. ", 0},
    {"GB 1-3 Pol. ", 0},
    {"GB 1-4 Pol. ", 0},
    {"GB Polarization ", 0},
    {"Nonpolar Sol. ", 0},
    {"LJ-14", 2},
    {"Coulomb-14", 0},
    {"LJC-14 q", 2},
    {"LJC Pairs NB", 2},
    {"LJ (SR)", 2},
    {"Buck.ham (SR)", 2},
    {"LJ ", 0},
    {"B.ham ", 0},
    {"Disper. corr.", 0},
    {"Coulomb (SR)", 0},
    {"Coul ", 0},
    {"RF excl.", 0},
    {"Coul. recip.", 0},
    {"LJ recip.", 0},
    {"DPD", 0},
    {"Polarization", 2},
    {"Water Pol.", 5},
    {"Thole Pol.", 4},
    {"Anharm. Pol.", 2},
    {"Position Rest.", 1},
    {"Flat-bottom posres", 1},
    {"Dis. Rest.", 2},
    {"D.R.Viol. (nm)", 0},
    {"Orient. Rest.", 2},
    {"Ori. R. RMSD", 0},
    {"Angle Rest.", 4},
    {"Angle Rest. Z", 2},
    {"Dih. Rest.", 4},
    {"Dih. Rest. Viol.", 0},
    {"Constraint", 2},
    {"Constr. No Conn.", 2},
    {"Settle", 3},
    {"Virtual site 1", 2},
    {"Virtual site 2", 3},
    {"Virtual site 2fd", 3},
    {"Virtual site 3", 4},
    {"Virtual site 3fd", 4},
    {"Virtual site 3fad", 4},
    {"Virtual site 3out", 4},
    {"Virtual site 4fd", 5},
    {"Virtual site 4fdn", 5},
    {"Virtual site N", 2},
    {"COM Pull En.", 0},
    {"Density fitting", 0},
    {"Quantum En.", 0},
    {"Potential", 0},
    {"Kinetic En.", 0},
    {"Total Energy", 0},
    {"Conserved En.", 0},
    {"Temperature", 0},
    {"Vir. Temp. ", 0},
    {"Pres. DC", 0},
    {"Pressure", 0},
    {"dH/dl constr.", 0},
    {"dVremain/dl", 0},
    {"dEkin/dl", 0},
    {"dVcoul/dl", 0},
    {"dVvdw/dl", 0},
    {"dVbonded/dl", 0},
    {"dVrestraint/dl", 0},
    {"dVtemperature/dl", 0},
};

// see `t_resinfo` in <GMX>/api/legacy/include/gromacs/topology/atoms.h
struct ResidueInfo {
    std::string name;
    int64_t residue_number;
    uint8_t insertion_code;
};

// see `t_atom` in <GMX>/api/legacy/include/gromacs/topology/atoms.h
struct AtomProperties {
    double mass;
    double charge;
    // Index into `residue_infos` in `Atoms`
    size_t residue_idx;
    chemfiles::optional<std::string> element_name = chemfiles::nullopt;
};

// see `t_atoms` in <GMX>/api/legacy/include/gromacs/topology/atoms.h
struct Atoms {
    std::vector<AtomProperties> atom_properties;
    std::vector<std::string> atom_names;
    std::vector<std::string> atom_types;
    std::vector<ResidueInfo> residue_infos;

    size_t size() const {
        size_t natoms = atom_properties.size();
        assert(natoms == atom_names.size());
        assert(natoms == atom_types.size());
        return natoms;
    }
};

// All the interactions for a given function type. This is stored as multiple
// interaction tuples which refer to a set of function parameters (skipped)
// and the atom indices.
// see <GMX>/api/legacy/include/gromacs/topology/idef.h
struct InteractionList {
    FunctionType function_type;
    // An interaction tuple contains the type of the interaction
    // (i.e. index to function parameters) first
    // followed by the number of interacting atoms.
    // This number depends on the function type (e.g. bond defines two atoms).
    std::vector<size_t> interaction_tuples;
    bool settle_done = false;

    bool empty() const { return interaction_tuples.empty(); }

    size_t size() const {
        // Number of entries per interaction (type_index + N * atom_index)
        size_t nentries = FUNCTION_TYPE_INFOS[function_type].natoms + 1;
        size_t size = interaction_tuples.size() / nentries;
        assert(size * nentries == interaction_tuples.size());
        return size;
    }

    chemfiles::span<const size_t> operator[](size_t i) const {
        size_t natoms = FUNCTION_TYPE_INFOS[function_type].natoms;
        // Stored as (type, atom_1, atom_2, ..., atom_N)
        // Skip first `i` tuples and the next type
        size_t begin = (natoms + 1) * i + 1;
        return chemfiles::span<const size_t>(interaction_tuples.data() + begin, natoms);
    }

    void add_settle_atoms() {
        assert(function_type == F_SETTLE);
        assert(!settle_done); // This can be called only once
        settle_done = true;
        // GROMACS: Settle used to only store the first atom: add the other two
        size_t size = interaction_tuples.size();
        interaction_tuples.resize(2 * size);
        auto& iatoms = interaction_tuples;
        for (int i = static_cast<int>(size) / 2 - 1; i >= 0; --i) {
            size_t j = static_cast<size_t>(i);
            iatoms[4 * j + 0] = iatoms[2 * j + 0];
            iatoms[4 * j + 1] = iatoms[2 * j + 1];
            iatoms[4 * j + 2] = iatoms[2 * j + 1] + 1;
            iatoms[4 * j + 3] = iatoms[2 * j + 1] + 2;
        }
    }
};

// GROMACS: List of interaction lists, one list for each interaction type.
// see <GMX>/api/legacy/include/gromacs/topology/idef.h
typedef std::array<chemfiles::optional<InteractionList>, F_NRE> InteractionLists;

// see `gmx_moltype_t` in <GMX>/api/legacy/include/gromacs/topology/topology.h
struct MoleculeType {
    std::string name;
    Atoms atoms;
    InteractionLists interaction_lists;
};

// `Count` in `SimulationAtomGroupType`
// see <GMX>/api/legacy/include/gromacs/topology/topology_enums.h
const size_t NR_GROUP_TYPES = 10;

using namespace chemfiles;

template <> const FormatMetadata& chemfiles::format_metadata<TPRFormat>() {
    static FormatMetadata metadata;
    metadata.name = "TPR";
    metadata.extension = ".tpr";
    metadata.description = "GROMACS TPR binary format";
    metadata.reference = "http://manual.gromacs.org/current/reference-manual/file-formats.html#tpr";

    metadata.read = true;
    metadata.write = false;
    metadata.memory = false;

    metadata.positions = false;
    metadata.velocities = false;
    metadata.unit_cell = true;
    metadata.atoms = true;
    metadata.bonds = false;
    metadata.residues = true;
    return metadata;
}

TPRFormat::TPRFormat(std::string path, File::Mode mode, File::Compression compression)
    : file_(std::move(path), mode) {
    if (compression != File::DEFAULT) {
        throw format_error("TPR format does not support compression");
    }
    if (mode != File::READ) {
        throw format_error("TPR format does not support write & append");
    }
}

size_t TPRFormat::nsteps() { return 1; }

void TPRFormat::read_step(size_t step, Frame& frame) {
    step_ = step;
    this->read(frame);
}

// Determine the size of all the interaction parameters for a function type.
// see `do_iparams` <GMX>/src/gromacs/fileio/tpxio.cpp
static size_t interaction_params_size(FunctionType ftype, size_t sizeof_real, int file_version) {
    switch (ftype) {
    case F_ANGLES:
    case F_G96ANGLES:
    case F_BONDS:
    case F_G96BONDS:
    case F_HARMONIC:
    case F_IDIHS:
        return 4 * sizeof_real;
    case F_RESTRANGLES:
        return 2 * sizeof_real;
    case F_LINEAR_ANGLES:
        return 4 * sizeof_real;
    case F_FENEBONDS:
        return 2 * sizeof_real;
    case F_RESTRBONDS:
        return 8 * sizeof_real;
    case F_TABBONDS:
    case F_TABBONDSNC:
    case F_TABANGLES:
    case F_TABDIHS:
        return 2 * sizeof_real + sizeof(int32_t);
    case F_CROSS_BOND_BONDS:
        return 3 * sizeof_real;
    case F_CROSS_BOND_ANGLES:
        return 4 * sizeof_real;
    case F_UREY_BRADLEY: {
        size_t size = 4 * sizeof_real;
        if (file_version >= 79) {
            size += 4 * sizeof_real;
        }
        return size;
    }
    case F_QUARTIC_ANGLES:
        return 6 * sizeof_real;
    case F_BHAM:
        return 3 * sizeof_real;
    case F_MORSE: {
        size_t size = 3 * sizeof_real;
        if (file_version >= 79) {
            size += 3 * sizeof_real;
        }
        return size;
    }
    case F_CUBICBONDS:
        return 3 * sizeof_real;
    case F_CONNBONDS:
        return 0;
    case F_POLARIZATION:
        return sizeof_real;
    case F_ANHARM_POL:
        return 3 * sizeof_real;
    case F_WATER_POL:
        return 6 * sizeof_real;
    case F_THOLE_POL: {
        size_t size = 3 * sizeof_real;
        if (file_version < tprv_RemoveTholeRfac) {
            size += sizeof_real;
        }
        return size;
    }
    case F_LJ:
        return 2 * sizeof_real;
    case F_LJ14:
        return 4 * sizeof_real;
    case F_LJC14_Q:
        return 5 * sizeof_real;
    case F_LJC_PAIRS_NB:
        return 4 * sizeof_real;
    case F_PDIHS:
    case F_PIDIHS:
    case F_ANGRES:
    case F_ANGRESZ:
        return 4 * sizeof_real + sizeof(int32_t);
    case F_RESTRDIHS:
        return 2 * sizeof_real;
    case F_DISRES:
        return 2 * sizeof(int32_t) + 4 * sizeof_real;
    case F_ORIRES:
        return 3 * sizeof(int32_t) + 3 * sizeof_real;
    case F_DIHRES: {
        size_t size = 3 * sizeof_real;
        if (file_version < 82) {
            size += 2 * sizeof(int32_t);
        }
        if (file_version >= 82) {
            size += 3 * sizeof_real;
        }
        return size;
    }
    case F_POSRES:
        return 4 * 3 * sizeof_real;
    case F_FBPOSRES:
        return sizeof(int32_t) + 5 * sizeof_real;
    case F_CBTDIHS:
        return NR_CBTDIHS * sizeof_real;
    case F_RBDIHS:
    case F_FOURDIHS:
        return 2 * NR_RBDIHS * sizeof_real;
    case F_CONSTR:
    case F_CONSTRNC:
        return 2 * sizeof_real;
    case F_SETTLE:
        return 2 * sizeof_real;
    case F_VSITE1:
        return 0;
    case F_VSITE2:
    case F_VSITE2FD:
        return 1 * sizeof_real;
    case F_VSITE3:
    case F_VSITE3FD:
    case F_VSITE3FAD:
        return 2 * sizeof_real;
    case F_VSITE3OUT:
    case F_VSITE4FD:
    case F_VSITE4FDN:
        return 3 * sizeof_real;
    case F_VSITEN:
        return sizeof(int32_t) + sizeof_real;
    case F_GB12_NOLONGERUSED:
    case F_GB13_NOLONGERUSED:
    case F_GB14_NOLONGERUSED: {
        size_t size = 0;
        if (file_version < 68) {
            size += 4 * sizeof_real;
        }
        if (file_version < tprv_RemoveImplicitSolvation) {
            size += 5 * sizeof_real;
        }
        return size;
    }
    case F_CMAP:
        return 2 * sizeof(int32_t);
    default:
        throw format_error("unknown function type {} ({})", ftype, FUNCTION_TYPE_INFOS[ftype].name);
    }
}

// see `do_ilists` in <GMX>/src/gromacs/fileio/tpxio.cpp
static InteractionLists read_interaction_lists(XDRFile& file, int file_version) {
    InteractionLists interaction_lists;
    for (size_t i = 0; i < F_NRE; ++i) {
        const FunctionType function_type = static_cast<FunctionType>(i);
        bool is_old_interaction = false;
        for (const auto function_type_update : FUNCTION_TYPE_UPDATES) {
            // Compare the read file version to the update table
            if ((file_version < function_type_update.file_version) &&
                (function_type == function_type_update.function_type)) {
                is_old_interaction = true;
                break;
            }
        }
        if (!is_old_interaction) {
            InteractionList ilist;

            ilist.function_type = function_type;
            size_t ninteraction_data = file.read_single_size_as_i32();
            ilist.interaction_tuples.reserve(ninteraction_data);
            for (size_t j = 0; j < ninteraction_data; ++j) {
                size_t idx = file.read_single_size_as_i32();
                ilist.interaction_tuples.emplace_back(idx);
            }

            if (file_version < 78 && ilist.function_type == F_SETTLE && !ilist.empty()) {
                ilist.add_settle_atoms();
            }

            interaction_lists[i] = ilist;
        }
    }
    return interaction_lists;
}

// Read all the different interations in the system. As only the types of
// interactions are used, the interaction parameters are skipped.
// see `do_ffparams` in <GMX>/src/gromacs/fileio/tpxio.cpp
static FFParams read_force_field_parameters(XDRFile& file, size_t sizeof_real, int file_version) {
    FFParams ffparams;
    ffparams.natom_types = file.read_single_i32();
    const size_t num_function_types = file.read_single_size_as_i32();
    ffparams.function_types.resize(num_function_types);

    // Read all function types
    file.read_i32(ffparams.function_types);

    if (file_version >= 66) {
        // Skip `reppow`: Repulsion power $p$ for VdW: $C12*r^-p$
        file.skip(sizeof(double));
    }

    // Skip `fudgeQQ`: Scaling factor $f$ for Coulomb 1-4: $f*q1*q2$
    file.skip(sizeof_real);

    // GROMACS explains:
    // Check whether all these function types are supported by the code.
    // In practice the code is backwards compatible, which means that the
    // numbering may have to be altered from old numbering to new numbering.
    for (auto& function_type : ffparams.function_types) {
        for (const auto& function_type_update : FUNCTION_TYPE_UPDATES) {
            // Compare the read file version to the update table
            if ((file_version < function_type_update.file_version) &&
                (function_type >= function_type_update.function_type)) {
                ++function_type;
            }
        }
        // Skip the force field parameters of the function
        size_t params_size = interaction_params_size(static_cast<FunctionType>(function_type),
                                                     sizeof_real, file_version);
        file.skip(params_size);
    }
    return ffparams;
}

void TPRFormat::read(Frame& frame) {
    const TprHeader header = read_header();
    frame.resize(header.natoms);

    // Now read the body of the TPR file
    // see `do_tpx_body` in <GMX>/src/gromacs/fileio/tpxio.cpp

    if (header.has_box) {
        read_box(frame, header);
    }

    if (header.ngroups_temperature_coupling > 0) {
        const size_t ngtc_size = header.ngroups_temperature_coupling * header.sizeof_real;
        if (header.file_version < 69) {
            // Skip some legacy entries
            file_.skip(ngtc_size);
        }
        // GROMACS: These used to be the Berendsen tcoupl_lambda's
        file_.skip(ngtc_size);
    }

    if (header.has_topology) {
        read_topology(frame, header);
    }

    step_++;
}

TPRFormat::TprHeader TPRFormat::read_header() {
    TprHeader header;

    const std::string version = file_.read_gmx_string();
    if (version.compare(0, 7, "VERSION") != 0) {
        throw format_error("unsupported file from a GROMACS version which is older than 2.0");
    }

    size_t precision = file_.read_single_size_as_i32();
    header.use_double = (precision == sizeof(double));
    if (precision != sizeof(float) && precision != sizeof(double)) {
        throw format_error("invalid precision {}, expected {} or {}", precision, sizeof(float),
                           sizeof(double));
    }

    header.sizeof_real = header.use_double ? sizeof(double) : sizeof(float);

    header.file_version = file_.read_single_i32();

    // GROMACS explains:
    // This is for backward compatibility with development versions 77-79
    // where the tag was, mistakenly, placed before the generation,
    // which would cause a segv instead of a proper error message
    // when reading the topology only from tpx with <77 code.
    std::string fileTag;
    if (header.file_version >= 77 && header.file_version <= 79) {
        fileTag = file_.read_gmx_string();
    }

    header.file_generation = file_.read_single_i32();

    if (header.file_version >= 81) {
        fileTag = file_.read_gmx_string();
    }
    if (header.file_version < 77 || header.file_version == 80) {
        // GROMACS: Versions before 77 don't have the tag, set it to release.
        // Version 80 is not handled by the current GROMACS implementation
        // but MDAnalysis sets the tag to release as well for version 80.
        // Version 80 was used by both 5.0-dev and 4.6-dev.
        fileTag = TPR_TAG_RELEASE;
    }

    // GROMACS explains:
    // We only support reading tpx files with the same tag as the code
    // or tpx files with the release tag and with lower version number.
    if (fileTag != TPR_TAG_RELEASE && header.file_version < TPR_VERSION) {
        throw format_error("TPR tag/version mismatch: reading file with version {}, tag '{}' with "
                           "program for version {}, tag '{}'",
                           header.file_version, fileTag, TPR_VERSION, TPR_TAG_RELEASE);
    }

    if (header.file_version > TPR_VERSION) {
        warning("TPR",
                "file version is from the future: implementation uses version {}, but got {}",
                TPR_VERSION, header.file_version);
    }

    // Assume `TopOnlyOK` is true, i.e. we need only the topology and not the input record.
    // This allows reading of future versions of the same generation.
    if ((header.file_version <= TPR_INCOMPATIBLE_VERSION) ||
        (header.file_generation > TPR_GENERATION)) {
        throw format_error("unable to read version {} with version {} program", header.file_version,
                           TPR_VERSION);
    }

    header.natoms = file_.read_single_size_as_i32();
    header.ngroups_temperature_coupling = file_.read_single_size_as_i32();

    if (header.file_version < 62) {
        // Skip some legacy entries
        file_.skip(sizeof(int) + header.sizeof_real);
    }
    if (header.file_version >= 79) {
        // Skip current value of the alchemical state
        file_.read_single_i32();
    }
    if (!header.use_double) {
        header.lambda = static_cast<double>(file_.read_single_f32());
    } else {
        header.lambda = file_.read_single_f64();
    }
    header.has_input_record = read_gmx_bool();
    header.has_topology = read_gmx_bool();
    header.has_positions = read_gmx_bool();
    header.has_velocities = read_gmx_bool();
    header.has_forces = read_gmx_bool();
    header.has_box = read_gmx_bool();

    if (header.file_version >= tprv_AddSizeField && header.file_generation >= 27) {
        // Skip size of the TPR body in bytes
        file_.read_single_i64();
    }

    if (header.file_generation > TPR_GENERATION && header.has_input_record) {
        // Trying to read a file from the future with an input record.
        // At this point in time, it's unknown what will be in the record.
        header.has_input_record = false;
    }

    if (header.file_version >= tprv_AddSizeField && header.file_generation >= 27) {
        header.body_convention = InMemory;
    } else {
        header.body_convention = FileIOXdr;
    }

    return header;
}

void TPRFormat::read_box(Frame& frame, const TprHeader& header) {
    const auto box = file_.read_gmx_box(header.use_double);
    frame.set_cell(box);

    if (header.file_version >= 51) {
        // Relative box vectors characteristic of the box shape
        // Skip unused 3*3 real matrix
        file_.skip(header.sizeof_real * 9);
    }

    // Box velocities for Parrinello-Rahman barostat
    // Skip unused 3*3 real matrix
    file_.skip(header.sizeof_real * 9);

    if (header.file_version < 56) {
        // Skip some legacy entries
        file_.skip(header.sizeof_real * 9);
    }
}

void TPRFormat::read_topology(Frame& frame, const TprHeader& header) {
    // Most of the used functions are defined in <GMX>/src/gromacs/fileio/tpxio.cpp
    // which is not repeated everytime in the following section.

    const std::vector<std::string> symbol_table = read_symbol_table(header);

    frame.set("name", read_symbol_table_entry(symbol_table));

    const FFParams ffparams =
        read_force_field_parameters(file_, header.sizeof_real, header.file_version);

    // Read the definitions of the different molecule types and
    // their atoms and residues.
    // see `do_moltype`
    auto read_molecule_types = [this, header, symbol_table]() -> std::vector<MoleculeType> {
        const size_t nmoltypes = file_.read_single_size_as_i32();
        std::vector<MoleculeType> molecule_types;
        molecule_types.reserve(nmoltypes);

        // Read the properties of single atom
        // see `do_atom`
        auto read_atom = [this, header]() -> AtomProperties {
            AtomProperties atom_prop;
            if (!header.use_double) {
                // Float
                atom_prop.mass = static_cast<double>(file_.read_single_f32());
                atom_prop.charge = static_cast<double>(file_.read_single_f32());
            } else {
                // Double
                atom_prop.mass = file_.read_single_f64();
                atom_prop.charge = file_.read_single_f64();
            }
            // Skip mass and charge for Free Energy calculations
            file_.skip(2 * header.sizeof_real);
            // Skip internal atom type
            if (header.body_convention == FileIOXdr) {
                file_.skip(2 * sizeof(uint32_t));
            } else {
                assert(header.body_convention == InMemory);
                file_.skip(2 * sizeof(uint16_t));
            }
            // Skip internal particle type
            file_.skip(sizeof(int32_t));
            atom_prop.residue_idx = file_.read_single_size_as_i32();
            const uint64_t atomnumber = static_cast<uint64_t>(file_.read_single_i32());
            for (const auto& kv : PERIODIC_TABLE) {
                // Determine the shortened element name from the atomnumber
                const auto& name = kv.first;
                const auto& atomic_data = kv.second;
                if (atomic_data.number && *atomic_data.number == atomnumber) {
                    atom_prop.element_name = name;
                }
            }
            return atom_prop;
        };

        // Read the definition of all the atoms in a molecule type
        // -> do_atoms
        auto read_atoms = [this, header, symbol_table, read_atom]() -> Atoms {
            Atoms atoms;
            const size_t natoms = file_.read_single_size_as_i32();
            const size_t nresidue_info = file_.read_single_size_as_i32();
            atoms.atom_properties.reserve(natoms);
            atoms.atom_names.reserve(natoms);
            atoms.atom_types.reserve(natoms);
            atoms.residue_infos.reserve(nresidue_info);
            for (size_t j = 0; j < natoms; ++j) {
                AtomProperties atom_prop = read_atom();
                atoms.atom_properties.emplace_back(atom_prop);
            }
            for (size_t j = 0; j < natoms; ++j) {
                auto name = read_symbol_table_entry(symbol_table);
                atoms.atom_names.emplace_back(name);
            }
            for (size_t j = 0; j < natoms; ++j) {
                auto type = read_symbol_table_entry(symbol_table);
                atoms.atom_types.emplace_back(type);
            }
            for (size_t j = 0; j < natoms; ++j) {
                // Skip the types for Free Energy calculations
                read_symbol_table_entry(symbol_table);
            }

            // Read the definition of residues
            // see `do_resinfo`
            for (size_t j = 0; j < nresidue_info; ++j) {
                std::string name = read_symbol_table_entry(symbol_table);
                uint8_t insertion_code = ' ';
                int64_t residue_number = static_cast<int64_t>(j) + 1;
                if (header.file_version >= 63) {
                    residue_number = static_cast<int64_t>(file_.read_single_i32());
                    insertion_code = read_gmx_uchar(header.body_convention);
                }
                ResidueInfo res_info = {name, residue_number, insertion_code};
                atoms.residue_infos.emplace_back(res_info);
            }

            return atoms;
        };

        for (size_t i = 0; i < nmoltypes; ++i) {
            MoleculeType moltype;
            moltype.name = read_symbol_table_entry(symbol_table);
            moltype.atoms = read_atoms();

            // Read the interaction lists
            moltype.interaction_lists = read_interaction_lists(file_, header.file_version);

            molecule_types.emplace_back(moltype);

            // GROMACS: Skip the obsolete charge group index
            // see `do_block`
            const size_t nblocks = file_.read_single_size_as_i32();
            file_.skip((nblocks + 1) * sizeof(int32_t));

            // Skip the exclusions
            size_t nlists = file_.read_single_size_as_i32();
            size_t nelements = file_.read_single_size_as_i32();
            file_.skip((nlists + 1 + nelements) * sizeof(int32_t));
        }
        return molecule_types;
    };
    const std::vector<MoleculeType> molecule_types = read_molecule_types();

    // Now create the actual topology of the system. The molecules are listed
    // sequentially after each other. Multiple molecules of the same type in
    // one row are aggregated in molecule blocks.
    // see `do_molblock` but most of the code is chemfiles specific
    size_t global_atom_idx = 0; // Number of atoms in the previous molecules
    const size_t nmolblocks = file_.read_single_size_as_i32();
    for (size_t i = 0; i < nmolblocks; ++i) {
        // Index of the molecule type read previously
        size_t moltype_idx = file_.read_single_size_as_i32();
        auto& moltype = molecule_types.at(moltype_idx);
        size_t nmolecules = file_.read_single_size_as_i32();
        const auto& atoms = moltype.atoms;
        for (size_t j = 0; j < nmolecules; ++j) {
            std::vector<chemfiles::Residue> residues_of_mol;
            residues_of_mol.reserve(atoms.residue_infos.size());
            for (const auto& res_info : atoms.residue_infos) {
                Residue residue = {res_info.name, res_info.residue_number};
                residue.set("insertion_code",
                            std::string(1, static_cast<char>(res_info.insertion_code)));
                residues_of_mol.emplace_back(residue);
            }
            for (size_t atom_idx = 0; atom_idx < atoms.size(); ++atom_idx) {
                auto& atom = frame[global_atom_idx + atom_idx];
                atom.set_name(atoms.atom_names[atom_idx]);
                atom.set("ff_type", atoms.atom_types[atom_idx]);
                auto& props = atoms.atom_properties[atom_idx];
                if (props.element_name) {
                    atom.set_type(*props.element_name);
                }
                atom.set_mass(props.mass);
                atom.set_charge(props.charge);

                if (props.residue_idx < moltype.atoms.residue_infos.size()) {
                    residues_of_mol[props.residue_idx].add_atom(global_atom_idx + atom_idx);
                } else {
                    throw format_error(
                        "residue index out of bounds, there are {} residues, got index {}",
                        moltype.atoms.residue_infos.size(), props.residue_idx);
                }
            }
            global_atom_idx += atoms.size();
            for (const auto& residue : residues_of_mol) {
                frame.add_residue(residue);
            }
        }

        // Skip number of atoms per molecule as this is already stored in the type
        file_.skip(sizeof(int32_t));
        for (size_t j = 0; j < 2; ++j) {
            size_t nposition_restraints = file_.read_single_size_as_i32();
            file_.skip(nposition_restraints * 3 * header.sizeof_real);
        }
    }

    const size_t natoms = file_.read_single_size_as_i32();
    assert(natoms == header.natoms);

    if (header.file_version >= tprv_IntermolecularBondeds) {
        bool has_intermolecular_bonds = read_gmx_bool(header.body_convention);
        if (has_intermolecular_bonds) {
            InteractionLists interaction_lists = read_interaction_lists(file_, header.file_version);
        }
    }

    // Skip atom types for old formats
    // see `do_atomtypes`
    if (header.file_version < tprv_RemoveAtomtypes) {
        size_t ntypes = file_.read_single_size_as_i32();
        if (header.file_version < tprv_RemoveImplicitSolvation) {
            file_.skip(3 * ntypes * header.sizeof_real);
        }
        file_.skip(ntypes * sizeof(int32_t));
        if (header.file_version >= 60 && header.file_version < tprv_RemoveImplicitSolvation) {
            file_.skip(2 * ntypes * header.sizeof_real);
        }
    }

    // Skip dihedral correction maps (CMAP)
    // see `do_cmap`
    if (header.file_version >= 65) {
        size_t ngrids = file_.read_single_size_as_i32();
        size_t grid_spacing = file_.read_single_size_as_i32();
        file_.skip(ngrids * grid_spacing * grid_spacing * 4 * header.sizeof_real);
    }

    // Skip atom groups
    // see `do_groups`
    {
        // Skip sizes of groups
        for (size_t i = 0; i < NR_GROUP_TYPES; ++i) {
            // see `do_grps`
            size_t group_size = file_.read_single_size_as_i32();
            file_.skip(group_size * sizeof(int32_t));
        }
        size_t ngroup_names = file_.read_single_size_as_i32();
        // Skip symbol table indices
        file_.skip(ngroup_names * sizeof(int32_t));
        // Skip group numbers
        for (size_t i = 0; i < NR_GROUP_TYPES; ++i) {
            size_t ngroup_numbers = file_.read_single_size_as_i32();
            if (header.body_convention == FileIOXdr) {
                file_.skip(ngroup_numbers * sizeof(uint32_t));
            } else {
                assert(header.body_convention == InMemory);
                file_.skip(ngroup_numbers * sizeof(uint8_t));
            }
        }
    }

    if (header.file_version >= tprv_StoreNonBondedInteractionExclusionGroup) {
        int64_t intermolecularExclusionGroupSize = file_.read_single_i64();
        if (intermolecularExclusionGroupSize < 0) {
            throw format_error("invalid intermolecular exclusion group size in TPR file: expected "
                               "a positive value, got {}",
                               intermolecularExclusionGroupSize);
        }
        uint64_t skip_size =
            static_cast<uint64_t>(intermolecularExclusionGroupSize) * sizeof(int32_t);
        file_.skip(skip_size);
    }
}

std::vector<std::string> TPRFormat::read_symbol_table(const TprHeader& header) {
    size_t nsymbols = file_.read_single_size_as_i32();
    std::vector<std::string> symbol_table;
    symbol_table.reserve(nsymbols);
    for (size_t i = 0; i < nsymbols; ++i) {
        symbol_table.emplace_back(read_gmx_string(header.body_convention));
    }
    return symbol_table;
}

const std::string& TPRFormat::read_symbol_table_entry(const std::vector<std::string>& table) {
    size_t idx = file_.read_single_size_as_i32();
    return table.at(idx);
}

std::string TPRFormat::read_gmx_string(TprBodyConvention body_convention) {
    if (body_convention == FileIOXdr) {
        return file_.read_gmx_string();
    } else {
        assert(body_convention == InMemory);
        // Read a non-XDR-compliant *long* GROMACS string
        // A long GROMACS string stores the length of the string as uint64 before
        // the string contents. The contents are *not* padded to make the size a
        // multiple of four bytes.

        // lenght without null terminator
        const uint64_t len = file_.read_single_u64();
        // next comes the contents of the string without padding
        std::vector<char> buf;
        buf.resize(static_cast<size_t>(len));
        file_.read_char(buf);
        return std::string(buf.begin(), buf.end());
    }
}

uint8_t TPRFormat::read_gmx_uchar(TprBodyConvention body_convention) {
    if (body_convention == FileIOXdr) {
        return static_cast<uint8_t>(file_.read_single_u32());
    } else {
        assert(body_convention == InMemory);
        return file_.read_single_u8();
    }
}

bool TPRFormat::read_gmx_bool(TprBodyConvention body_convention) {
    if (body_convention == FileIOXdr) {
        return file_.read_single_i32() != 0;
    } else {
        assert(body_convention == InMemory);
        return file_.read_single_u8() != 0;
    }
}
