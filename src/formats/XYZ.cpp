// Chemfiles, a modern library for chemistry file reading and writing
// Copyright (C) Guillaume Fraux and contributors -- BSD license

#include <cassert>
#include <cstdint>
#include <map>
#include <array>
#include <string>
#include <vector>
#include <exception>

#include "chemfiles/File.hpp"
#include "chemfiles/Format.hpp"
#include "chemfiles/Atom.hpp"
#include "chemfiles/Frame.hpp"
#include "chemfiles/Topology.hpp"
#include "chemfiles/Property.hpp"

#include "chemfiles/types.hpp"
#include "chemfiles/parse.hpp"
#include "chemfiles/error_fmt.hpp"
#include "chemfiles/external/optional.hpp"
#include "chemfiles/warnings.hpp"

#include "chemfiles/formats/XYZ.hpp"

using namespace chemfiles;

/// Parse the Lattice="..." in extended XYZ
static UnitCell parse_cell(string_view lattice);

struct extended_property {
    std::string name;
    Property::Kind type;
};

/// Type for a list of additional atomic properties in extended XYZ
using properties_list_t = std::vector<extended_property>;

/// Read the extended XYZ properties from the line, set frame properties
/// directly and return the list of atomic properties to read
static properties_list_t read_extended_comment_line(string_view line, Frame& frame);

/// Read the properties in the list form the line and set them on the atom
static void read_atomic_properties(const properties_list_t& properties, string_view line, Atom& atom);

/// Generate the extended XYZ comment line for the given frame
static std::string write_extended_comment_line(const Frame& frame);

template<> const FormatMetadata& chemfiles::format_metadata<XYZFormat>() {
    static FormatMetadata metadata;
    metadata.name = "XYZ";
    metadata.extension = ".xyz";
    metadata.description = "XYZ text format";
    return metadata;
}

void XYZFormat::read_next(Frame& frame) {
    size_t natoms = 0;
    try {
        natoms = parse<size_t>(file_.readline());
    } catch (const std::exception& e) {
        throw format_error("can not read next step as XYZ: {}", e.what());
    }

    auto properties = read_extended_comment_line(file_.readline(), frame);

    frame.reserve(natoms);
    for (size_t i=0; i<natoms; i++) {
        auto line = file_.readline();
        double x = 0, y = 0, z = 0;
        std::string name;
        auto count = scan(line, name, x, y, z);
        auto atom = Atom(std::move(name));
        read_atomic_properties(properties, line.substr(count), atom);
        frame.add_atom(std::move(atom), Vector3D(x, y, z));
    }
}

void XYZFormat::write_next(const Frame& frame) {
    auto& topology = frame.topology();
    auto& positions = frame.positions();
    assert(frame.size() == topology.size());

    file_.print("{}\n", frame.size());
    file_.print("{}\n", write_extended_comment_line(frame));

    for (size_t i = 0; i < frame.size(); i++) {
        auto name = topology[i].name();
        if (name.empty()) {
            name = "X";
        }
        file_.print("{} {:g} {:g} {:g}\n",
            name, positions[i][0], positions[i][1], positions[i][2]
        );
    }
}

optional<uint64_t> XYZFormat::forward() {
    auto position = file_.tellpos();
    size_t natoms = 0;
    try {
        natoms = parse<size_t>(file_.readline());
    } catch (const Error&) {
        // We could not read an integer, so give up here
        return nullopt;
    }

    for (size_t i=0; i<natoms + 1; i++) {
        if (file_.eof()) {
            throw format_error(
                "XYZ format: not enough lines at step {} (expected {}, got {})",
                current_forward_step_, natoms + 2, i + 1
            );
        }

        file_.readline();
    }

    current_forward_step_++;

    return position;
}

/*****************************************************************************/
/* End of the main XYZ format implementation. The code below implements the  */
/* extended XYZ format/convention. This format is 100% backward compatible   */
/* with normal XYZ. It adds data in two places: after the atomic positions   */
/* and in the comment line. The comment line contains key/values pairs       */
/* describing frame-level properties, as well as two special keys: Lattice   */
/* stores the unit cell matrix, and 'Properties' describe the name and type  */
/* of any additional atom-level data.                                        */
/*****************************************************************************/

// Should we surround `s` with quotation marks when using it as property name?
static bool should_be_quoted(string_view s) {
    for (auto c: s) {
        // TODO: ASE also allow [] {} and () to function as quotes. This should
        // be updated when a specification is agreed on.
        if (is_whitespace(c) || c == '=' || c == '\'' || c == '"') {
            return true;
        }
    }
    return false;
}

static bool contains_single_quote(string_view s) {
    for (auto c: s) {
        if (c == '\'') {
            return true;
        }
    }
    return false;
}

static bool contains_double_quote(string_view s) {
    for (auto c: s) {
        if (c == '"') {
            return true;
        }
    }
    return false;
}

std::string write_extended_comment_line(const Frame& frame) {
    std::string result;
    // TODO: store more properties? Atom-level properties are a good idea, but
    // we don't have guarantee that they exist for all atoms. Residue
    // information could also be nice (resid/resname).
    result += "Properties=species:S:1:pos:R:3";

    // support for Lattice
    if (frame.cell().shape() != UnitCell::INFINITE) {
        auto matrix = frame.cell().matrix();
        // set small elements to 0
        for (size_t i=0; i<3; i++) {
            for (size_t j=0; j<3; j++) {
                if (matrix[i][j] < 1e-12) {
                    matrix[i][j] = 0;
                }
            }
        }
        result += fmt::format(" Lattice=\"{:g} {:g} {:g} {:g} {:g} {:g} {:g} {:g} {:g}\"",
            matrix[0][0], matrix[1][0], matrix[2][0],
            matrix[0][1], matrix[1][1], matrix[2][1],
            matrix[0][2], matrix[1][2], matrix[2][2]
        );
    }

    // sort properties to have reproducible output
    auto sorted_properties = std::map<string_view, Property>{
        frame.properties().begin(), frame.properties().end()
    };
    // support for generic frame properties
    for (const auto& it: sorted_properties) {
        if (should_be_quoted(it.first)) {
            // quote the string
            if (!contains_double_quote(it.first)) {
                result += fmt::format(" \"{}\"=", it.first);
            } else if (!contains_single_quote(it.first)) {
                result += fmt::format(" '{}'=", it.first);
            } else {
                 warning("Extended XYZ", "frame property '{}' can not be represented, skipping", it.first);
                 continue;
            }
        } else {
            result += fmt::format(" {}=", it.first);
        }

        switch (it.second.kind()) {
        case Property::STRING:
            result += fmt::format("\"{}\"", it.second.as_string());
            break;
        case Property::BOOL:
            result += fmt::format("{}", it.second.as_bool() ? 'T' : 'F');
            break;
        case Property::DOUBLE:
            result += fmt::format("{:g}", it.second.as_double());
            break;
        case Property::VECTOR3D:
            {
                auto v = it.second.as_vector3d();
                result += fmt::format("\"{:g} {:g} {:g}\"", v[0], v[1], v[2]);
            }
            break;
        }
    }
    return result;
}

/// mapping name=>properties values. The use of string_view is safe since the
/// original string_view (as returned by file.readline()) will stay alive longer
/// than this map.
using extended_xyz_properties_map_t = std::unordered_map<string_view, Property>;

/// A simple parser for the extended XYZ comment line. This parser bails out as
/// soons as possible if the line does not seems to follow extended XYZ
/// convention.
///
/// This line should contains key/values pairs separated by spaces, while the
/// key and the value are separated by an = sign. If the value contains spaces,
/// it is then quoted, both single quotes and double quotes are accepted here.
/// Example: Properties=species:S:1:pos:R:3:bad:I:1 Lattice="3 0 0 0 4 0 0 0 5" name='simple file'
struct extended_xyz_parser {
public:
    extended_xyz_parser(string_view line): current_(line.data()), end_(line.data() + line.size()) {}
    ~extended_xyz_parser() = default;
    extended_xyz_parser(extended_xyz_parser&&) = delete;
    extended_xyz_parser(const extended_xyz_parser&) = delete;
    extended_xyz_parser& operator=(extended_xyz_parser&&) = delete;
    extended_xyz_parser& operator=(const extended_xyz_parser&) = delete;

    extended_xyz_properties_map_t parse() {
        extended_xyz_properties_map_t properties;
        while (!done()) {
            skip_whitespace();
            auto name = next_substring();
            if (!name.empty() && current() == '=') {
                advance();
            } else {
                // properties without associated value are set to True by ASE
                properties.emplace(name, true);
                continue;
            }

            auto value = next_substring();
            if (!is_whitespace(current()) && !done()) {
                // missing a space after the value, bail out here
                warning("Extended XYZ", "expected whitespace after the value for {}, got {}", name, current());
                break;
            }

            properties.emplace(name, value.to_string());
        }
        return properties;
    }

private:
    void skip_whitespace() {
        while (!done()) {
            if (is_whitespace(current())) {
                advance();
            } else {
                return;
            }
        }
    }

    /// Get a string, either quoted and possibly containing spaces; or unquoted.
    /// Unquoted strings ends on a space or '=' sign.
    string_view next_substring() {
        bool check_for_quote = false;
        char quote = '\0';
        if (current() == '"' || current() == '\'') {
            check_for_quote = true;
            quote = current();
            advance();
        }

        auto start = current_;
        size_t size = 0;
        while (!done()) {
            if (check_for_quote && current() == quote) {
                // end of quoted value
                check_for_quote = false;
                advance();
                break;
            } else if (!check_for_quote && (is_whitespace(current()) || current() == '=')) {
                // end of non-quoted value
                break;
            } else {
                advance();
                size++;
            }
        }

        auto value = string_view(start, size);
        if (check_for_quote) {
            // we found an initial quote, but no final one
            assert(done());
            warning("Extended XYZ", "missing final quote after {}", value);
        }
        return value;
    }

    // are we done with the input?
    bool done() const {
        return current_ == end_;
    }

    // Advance the pointer and return the new character
    void advance() {
        if (!done()) {
            current_++;
        }
    }

    char current() {
        if (done()) {
            return '\0';
        } else {
            return *current_;
        }
    }

    const char* current_;
    const char* end_;
};


static UnitCell parse_cell(string_view lattice) {
    auto matrix = Matrix3D::zero();
    scan(lattice,
        matrix[0][0], matrix[1][0], matrix[2][0],
        matrix[0][1], matrix[1][1], matrix[2][1],
        matrix[0][2], matrix[1][2], matrix[2][2]
    );
    return UnitCell(matrix);
}

/// Parse extended XYZ properties format string
///
/// Format is "[NAME:TYPE:NCOLS]...", e.g. "species:S:1:pos:R:3:CS:R:4".
/// NAME is the name of the property.
/// TYPE is one of R, I, S, L for real, integer, string and logical.
/// NCOLS is number of columns for that property.
///
/// The standard XYZ properties ("species:S:1:pos:R:3".) are checked, but not
/// returned in the property list.
///
/// When converting atomic properties as described by the 'Properties'
/// value, the followig rules are used:
///
/// - Properties of type L:1 are maped to boolean values
/// - Properties of type L:N are maped to N separate boolean properties,
///   named `$name_$i`
///
/// - Properties of type R:1 and I:1 are maped to double values
/// - Properties of type R:3 and I:3 are maped to Vector3D values
/// - Properties of type R:N and I:N are maped to to N separate double
///   properties, named `$name_$i`
///
/// - Properties of type S:1 are maped to string values
/// - Properties of type S:N are maped to N separate string properties,
///   named `$name_$i`
static properties_list_t parse_property_list(string_view input) {
    if (input.substr(0, 19) != "species:S:1:pos:R:3") {
        warning("Extended XYZ", "ignoring non-standard Properties='{}', should start with 'species:S:1:pos:R:3'", input);
        return {};
    }

    // saving the initial input for error messages
    auto list = input;
    if (list.size() <= 20) {
        return {};
    }
    list.remove_prefix(20);
    properties_list_t properties;

    auto splitted = split(list, ':');
    if (splitted.size() % 3 != 0) {
        warning("Extended XYZ", "ignoring invalid Properties='{}'", input);
        return {};
    }

    auto count = splitted.size() / 3;
    for (size_t i=0; i<count; i++) {
        auto name = splitted[3 * i].to_string();

        auto type_str = splitted[3 * i + 1];
        Property::Kind type;
        if (type_str == "R" || type_str == "I") {
            type = Property::DOUBLE;
        } else if (type_str == "S") {
            type = Property::STRING;
        } else if (type_str == "L") {
            type = Property::BOOL;
        } else {
            warning("Extended XYZ", "invalid type name for {} in Properties='{}'", name, input);
            continue;
        }

        size_t repeat = 0;
        try {
            repeat = parse<size_t>(splitted[3 * i + 2]);
        } catch (const Error&) {
            warning("Extended XYZ", "invalid type repeat for {} in Properties='{}'", name, input);
            continue;
        }

        if (repeat == 3 && type == Property::DOUBLE) {
            type = Property::VECTOR3D;
            properties.emplace_back(extended_property{name, type});
            continue;
        }

        if (repeat == 0) {
            warning("Extended XYZ", "invalid type repeat for {} in Properties='{}'", name, input);
            continue;
        } else if (repeat == 1) {
            properties.emplace_back(extended_property{name, type});
        } else {
            for (size_t j=0; j<repeat; j++) {
                properties.emplace_back(extended_property{
                    fmt::format("{}_{}", name, j), type
                });
            }
        }
    }

    return properties;
}

properties_list_t read_extended_comment_line(string_view line, Frame& frame) {
    // only try to parse as extended XYZ if `Properties` is defined as expected
    if (line.find("species:S:1:pos:R:3") == std::string::npos) {
        return {};
    }
    auto properties = extended_xyz_parser(line).parse();

    for (const auto& it: properties) {
        auto name = it.first;
        if (name == "Lattice" || name == "Properties") {
            continue;
        }
        frame.set(name.to_string(), std::move(it.second));
    }

    if (properties.count("Lattice") == 1) {
        frame.set_cell(parse_cell(properties.at("Lattice").as_string()));
    }

    if (properties.count("Properties") == 1) {
        return parse_property_list(properties.at("Properties").as_string());
    } else {
        return {};
    }
}

// This function throws `chemfiles::Error` if the values are not convertible to
// the expected type. If the files contains a valid `Properties=...`
// description,throwing errors if the rest of the files does not follow the
// description is fair game.
void read_atomic_properties(const properties_list_t& properties, string_view line, Atom& atom) {
    for (const auto& property: properties) {
        if (property.type == Property::STRING) {
            std::string value;
            auto count = scan(line, value);
            line.remove_prefix(count);
            atom.set(property.name, std::move(value));
        } else if (property.type == Property::BOOL) {
            std::string value;
            auto count = scan(line, value);
            line.remove_prefix(count);
            tolower(value);
            if (value == "t" || value == "true") {
                atom.set(property.name, true);
            } else if (value == "f" || value == "false") {
                atom.set(property.name, false);
            } else {
                throw error("invalid value for boolean '{}'", value);
            }
        } else if (property.type == Property::DOUBLE) {
            double value;
            auto count = scan(line, value);
            line.remove_prefix(count);
            atom.set(property.name, value);
        }  else if (property.type == Property::VECTOR3D) {
            Vector3D value;
            auto count = scan(line, value[0], value[1], value[2]);
            line.remove_prefix(count);
            atom.set(property.name, std::move(value));
        } else {
            unreachable();
        }
    }
}
