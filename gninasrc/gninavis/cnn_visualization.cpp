#include <iostream>
#include <iomanip>
#include <stdlib.h>
#include <set>
#include <openbabel/obconversion.h>
#include <openbabel/obiter.h>
#include <openbabel/mol.h>
#include "cnn_visualization.hpp"
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include "cnn_scorer.h"
#include "molgetter.h"
#include "obmolopener.h"
#include "model.h"
#include "parse_pdbqt.h"
#include "parsing.h"
#include <GraphMol/Subgraphs/Subgraphs.h>
#include <GraphMol/RDKitBase.h>
#include <RDGeneral/Invariant.h>
#include <GraphMol/FileParsers/FileParsers.h>
#include <GraphMol/MolOps.h>

using namespace OpenBabel;

cnn_visualization::cnn_visualization(const vis_options &viso,
        const cnn_options &copts, const vec &c):
            visopts(viso), cnnopts(copts), center(c) {
    if (visopts.gpu > -1) {
        caffe::Caffe::SetDevice(visopts.gpu);
        caffe::Caffe::set_mode(caffe::Caffe::GPU);
    }

    OBConversion conv;
    obmol_opener opener;

    try {
        opener.openForInput(conv, visopts.ligand_name);
        conv.Read(&lig_mol);
        opener.openForInput(conv, visopts.receptor_name);
        conv.Read(&rec_mol);
    } catch (file_error& e) {
        std::cout << "Could not open \"" << e.name.string()
                << "\" for reading\n";
        exit(1);
    }

    std::ifstream lig_fstream(visopts.ligand_name);
    std::ifstream rec_fstream(visopts.receptor_name);

    std::stringstream buffer;
    buffer << lig_fstream.rdbuf();
    original_lig_string = buffer.str();

    buffer.str("");
    buffer << rec_fstream.rdbuf();
    original_rec_string = buffer.str();

    setup();
}

void cnn_visualization::lrp() {

    std::cout << "Doing LRP...\n";

    std::stringstream rec_stream(rec_string);
    model receptor = parse_receptor_pdbqt("", rec_stream);
    CNNScorer scorer(cnnopts, center, receptor);

    std::stringstream lig_stream(lig_string);
    model ligand = parse_ligand_stream_pdbqt("", lig_stream);

    receptor.append(ligand);

    float aff;

    if(visopts.zero_values)
    {
        std::cout << "Only propagating from zero nodes.\n";
    }
    scorer.lrp(receptor, visopts.layer_to_ignore, visopts.zero_values);
    std::unordered_map<std::string, float> lig_scores = scorer.get_scores_per_atom(false, true);
    std::unordered_map<std::string, float> rec_scores = scorer.get_scores_per_atom(true, true);

    //don't write pdbqt output if zeroing values, will be just zeroes
    if(!visopts.zero_values)
    {
        write_scores(lig_scores, false, "lrp");
        write_scores(rec_scores, true, "lrp");
    }

    if (visopts.outputdx) 
    {
        float scale = 1.0;
        boost::filesystem::path lig_name_path(visopts.ligand_name);
        std::string lig_prefix = "lrp_" + lig_name_path.stem().string();
        scorer.outputDX(lig_prefix, scale, true, visopts.layer_to_ignore);
    }
    std::cout << "LRP finished.\n";
}

void cnn_visualization::gradient_vis() {

    std::cout << "Doing gradient...\n";

    std::stringstream rec_stream(rec_string);
    model receptor = parse_receptor_pdbqt("", rec_stream);
    CNNScorer scorer(cnnopts, center, receptor);

    std::stringstream lig_stream(lig_string);
    model ligand = parse_ligand_stream_pdbqt("", lig_stream);

    receptor.append(ligand);

    boost::filesystem::path rec_name_path(visopts.receptor_name);
    std::string rec_output_name = "gradient_" + rec_name_path.stem().string() + ".xyz";

    boost::filesystem::path lig_name_path(visopts.ligand_name);
    std::string lig_prefix = "gradient_" + lig_name_path.stem().string();
    std::string lig_output_name = lig_prefix + ".xyz";

    if(visopts.layer_to_ignore.length() > 0)
    {
        std::cout << "Ignoring layer: \"" << visopts.layer_to_ignore << "\"\n";
        scorer.gradient_setup(receptor, rec_output_name, lig_output_name, visopts.layer_to_ignore);
    }
    else
    {
        scorer.gradient_setup(receptor, rec_output_name, lig_output_name);
    }
    std::unordered_map<std::string, float> lig_scores = scorer.get_scores_per_atom(false, false);
    std::unordered_map<std::string, float> rec_scores = scorer.get_scores_per_atom(true, false);

    write_scores(lig_scores, false, "gradient");
    write_scores(rec_scores, true, "gradient");

    if (visopts.outputdx)
    {
        float scale = 1.0;
        scorer.outputDX(lig_prefix, scale);
    }
    std::cout << "Gradient finished.\n";
}


void cnn_visualization::setup(){
    if (visopts.verbose) {
        print();
    }

    process_molecules();

    std::stringstream rec_stream(rec_string);
    unmodified_receptor = parse_receptor_pdbqt("", rec_stream);
    CNNScorer base_scorer(cnnopts, center, unmodified_receptor);

    std::stringstream lig_stream(lig_string);
    unmodified_ligand = parse_ligand_stream_pdbqt("", lig_stream);
    model temp_rec = unmodified_receptor;

    temp_rec.append(unmodified_ligand);
    if(visopts.target == "pose")
    {
        original_score = base_scorer.score(temp_rec, true);
        std::cout << "CNN SCORE: " << original_score << "\n\n";
    }
    else if(visopts.target == "affinity")
    {
        float aff;
        original_score = base_scorer.score(temp_rec, false, aff, true);
        original_score = aff;
        std::cout << "AFF: " << original_score << "\n\n";
    }
    else 
    {
        std::cout << "Unknown scoring target\n";
        return;
    }
}


void cnn_visualization::masking() {

    std::cout << "Doing masking...\n";
    if (!visopts.skip_receptor_output) {
        remove_residues();
    }

    if (!visopts.skip_ligand_output) {
        remove_ligand_atoms();
    }

    std::cout << "Masking finished.\n";
}

void cnn_visualization::print() {
    std::cout << "ligand_name: " << visopts.ligand_name << '\n';
    std::cout << "receptor_name: " << visopts.receptor_name << '\n';
    std::cout << "cnn_model: " << cnnopts.cnn_model << '\n';
    std::cout << "cnn_weights: " << cnnopts.cnn_weights << '\n';
    std::cout << "box_size: " << visopts.box_size << '\n';
    std::cout << "skip_receptor_output: " << visopts.skip_receptor_output << '\n';
    std::cout << "skip_ligand_output: " << visopts.skip_ligand_output << '\n';
    std::cout << "frags_only: " << visopts.frags_only << '\n';
    std::cout << "atoms_only: " << visopts.atoms_only << '\n';
    std::cout << "verbose: " << visopts.verbose << "\n\n";
}

std::string cnn_visualization::modify_pdbqt(const std::unordered_set<std::string> &atoms_to_remove,
        bool isRec) {
    std::string mol_string;
    OBMol mol;

    if (isRec) {
        mol_string = rec_string;
        mol = rec_mol;
    } else {
        mol_string = lig_string;
        mol = lig_mol;
    }

    std::stringstream ss;
    std::stringstream mol_stream(mol_string);

    ss << "ROOT\n"; 

    bool list_ended = false;
    std::string line;
    while (std::getline(mol_stream, line)) {
        if ((line.find("ATOM") < std::string::npos)) {

            std::string xyz = get_xyz(line);
            if(atoms_to_remove.find(xyz) == atoms_to_remove.end())
            {
                ss << line << '\n';
            }
        }
    }

    ss << "ENDROOT\n";
    ss << "TORSDOF 0\n";

    if (visopts.verbose) {
        std::cout << "]\n";
    }

    return ss.str();
}


std::string cnn_visualization::modify_pdbqt(const std::unordered_set<int> &atoms_to_remove,
        bool isRec) {
    std::string mol_string;
    OBMol mol;

    if (isRec) {
        mol_string = rec_string;
        mol = rec_mol;
    } else {
        mol_string = lig_string;
        mol = lig_mol;
    }

    std::stringstream ss;
    std::stringstream mol_stream(mol_string);

    ss << "ROOT\n"; 

    bool list_ended = false;
    std::string line;
    while (std::getline(mol_stream, line)) {
        if ((line.find("ATOM") < std::string::npos)) {
            std::string index_string = line.substr(7, 5);
            int atom_index = std::stoi(index_string);
            bool in_list = false;

                if(atoms_to_remove.find(atom_index) != atoms_to_remove.end())
                {
                    in_list = true;
                }

            if (!in_list)
            {
                ss << line << '\n';
            }
        }
    }

    ss << "ENDROOT\n";
    ss << "TORSDOF 0\n";

    if (visopts.verbose) {
        std::cout << "]\n";
    }

    return ss.str();
}

//add hydrogens with openbabel, generate PDBQT
//files for removal
void cnn_visualization::process_molecules() {
    rec_mol.AddHydrogens();

    lig_mol.AddHydrogens(true, false, 7.4); //add only polar hydrogens

    OBConversion conv;

    conv.AddOption("r", OBConversion::OUTOPTIONS); //treat as rigid
    conv.AddOption("c", OBConversion::OUTOPTIONS); //combine rotatable portions of molecule
    conv.AddOption("p", OBConversion::OUTOPTIONS);
    conv.SetOutFormat("PDBQT"); //use pdbqt to make passing to parse_pdbqt possible

    //generate base ligand pdbqt string
    std::string temp_lig_string = conv.WriteString(&lig_mol);
    std::stringstream lig_stream;
    if(temp_lig_string.find("ROOT") == std::string::npos)
    {
        lig_stream << "ROOT\n";
    }
    lig_stream << temp_lig_string;
    if(temp_lig_string.find("ENDROOT") == std::string::npos)
    {
        lig_stream << "ENDROOT\n";
    }
    if(temp_lig_string.find("TORSDOF") == std::string::npos)
    {
        lig_stream << "TORSDOF 0";
    }
    lig_string = lig_stream.str();

    //generate base receptor pdbqt string
    std::string temp_rec_string = conv.WriteString(&rec_mol);
    std::stringstream rec_stream;
    rec_stream << "ROOT\n";
    rec_stream << temp_rec_string;
    rec_stream << "ENDROOT\n" << "TORSDOF 0";
    rec_string = rec_stream.str();

    OBMol lig_copy = lig_mol; //Center() will change atom coordinates, so use copy

    vector3 cen = lig_copy.Center(0);
    cenCoords[0] = cen.GetX();
    cenCoords[1] = cen.GetY();
    cenCoords[2] = cen.GetZ();
}

//scores provided receptor string against unmodified ligand
float cnn_visualization::score_modified_receptor(
        const std::string &modified_rec_string) {
    std::stringstream rec_stream(modified_rec_string);
    std::stringstream lig_stream(lig_string);

    model m = parse_receptor_pdbqt("", rec_stream);

    CNNScorer cnn_scorer(cnnopts, center, m);

    model l = parse_ligand_stream_pdbqt("", lig_stream);
    m.append(l);

    float aff;
    float score_val = cnn_scorer.score(m, true, aff);

    //use affinity instead of cnn score if required
    if (visopts.target == "affinity")
    {
        score_val =  aff;
    }

    if (visopts.verbose) {
        std::cout << "SCORE: " << score_val << '\n';
    }

    return score_val;
}

//scores provided ligand string against unmodified ligand
float cnn_visualization::score_modified_ligand(const std::string &mol_string) {

    //check if any ATOM fields present (removed fragment might be whole
    //molecule)
    if (mol_string.find("ATOM") == std::string::npos)
    {
        return 0;
    }

    std::stringstream lig_stream(mol_string);
    std::stringstream rec_stream(rec_string);

    model temp = unmodified_receptor;
    static CNNScorer cnn_scorer; //share same scorer across all ligands

    static bool first = true;
    if (first) {
        cnn_scorer = CNNScorer(cnnopts, center, temp);
        first = false;
    }

    model l = parse_ligand_stream_pdbqt("", lig_stream);
    temp.append(l);

    float aff;
    float score_val = cnn_scorer.score(temp, true, aff, true);
    if (visopts.verbose) {
        std::cout << "SCORE: " << score_val << '\n';
    }

    if (visopts.target == "pose")
    {
        return score_val;
    }
    else if (visopts.target == "affinity")
    {
        return aff;
    }
}

//wrapper for vector, converts to xyz map
void cnn_visualization::write_scores(const std::vector<float> &scores,
        bool isRec, std::string method)
{
    std::unordered_map<std::string, float> score_map;
    for(int i = 0; i < scores.size(); i++)
    {
        //only include nonzero scores in set:
        if(scores[i] != 0.0000)
        {
            score_map[get_xyz_from_index(i, isRec)] = scores[i];
        }
    }
    write_scores(score_map, isRec, method);
}

//map: xyz coordinates concatenated:scores
void cnn_visualization::write_scores(std::unordered_map<std::string, float> scores,
        bool isRec, std::string method) {
    std::string file_name;
    std::string mol_string;

    if (isRec) {
        file_name = visopts.receptor_name;
        mol_string = rec_string;
    } else {
        file_name = visopts.ligand_name;
        mol_string = lig_string;
    }

    boost::filesystem::path file_name_path(file_name);
    file_name = method + "_" + file_name_path.stem().string() + ".pdbqt";
    std::string extended_file_name = file_name + ".ext";

    for(int i = 0; i < 2; i++)
    {
        std::ofstream curr_out_file;
        if(i  == 0)
        {
            curr_out_file.open(file_name);
        }
        else
        {
            curr_out_file.open(extended_file_name);
        }

        curr_out_file << "VIS METHOD: " << method << '\n';
        if(method == "masking")
        {
            curr_out_file << "MASKING TARGET: " << visopts.target << '\n';
        }
        if(method == "gradient" || method == "lrp")
        {
            curr_out_file << "LAYER IGNORED: " << visopts.layer_to_ignore << '\n';
        }

        if(visopts.target == "pose")
        {
            curr_out_file << "POSE SCORE: " << original_score << '\n';
        }
        if(visopts.target == "affinity")
        {
            curr_out_file << "AFFINITY SCORE: " << original_score << '\n';
        }
        else
        {
            curr_out_file << "CNN SCORE: " << original_score << '\n';
        }

        curr_out_file << "MODEL: " << cnnopts.cnn_model << '\n';
        curr_out_file << "WEIGHTS: " << cnnopts.cnn_weights << '\n';

        std::stringstream mol_stream(mol_string);
        std::string line;
        std::string index_string;
        int atom_index;
        std::stringstream score_stream;
        std::string score_string;

        float score_sum = 0;
        int found_count = 0;
        std::unordered_map<std::string, bool> found_scores;
        for(auto i: scores)
        {
            found_scores[i.first] = false;
        }

        while (std::getline(mol_stream, line)) {
            if ((line.find("ATOM") < std::string::npos)
                    || (line.find("HETATM") < std::string::npos)) {
                score_stream.str(""); //clear stream for next score
                index_string = line.substr(6, 5);
                std::string elem = line.substr(77,2);
                atom_index = std::stoi(index_string);

                std::string xyz = get_xyz(line);

                float score;
                //if present, assign score
                if ( scores.find(xyz) == scores.end())
                {
                    score = 0.0;
                }

                else
                {
                    score = scores[xyz];
                    found_count++;
                    found_scores[xyz] = true;
                }

                score_stream << std::fixed << std::setprecision(5) << score;
                curr_out_file << line.substr(0, 61);
                score_string = score_stream.str();
                if(i == 0)
                {
                    curr_out_file.width(5);
                    score_string.resize(5);
                }
                else
                {
                    curr_out_file.width(7);
                    score_string.resize(7);
                }
                curr_out_file.width(5);
                curr_out_file.fill('.');
                curr_out_file << std::right << score_string;
                curr_out_file << line.substr(66) << '\n'; 
                }
            else {
                curr_out_file << line << '\n';
            }
        }

        assert(found_count == scores.size());

        //print any scores not output
        /*
        for(auto i : found_scores)
        {
            if(i.second == false)
            {
                std::cout << "missed: " << i.first << '\n';
            }
        }
        */

    }
}

//returns false if not at least one atom within range of center of ligand
bool cnn_visualization::check_in_range(const std::unordered_set<std::string> &atom_xyzs) {
    float x = cenCoords[0];
    float y = cenCoords[1];
    float z = cenCoords[2];

    float allowed_dist = visopts.box_size / 2;
    int num_atoms = rec_mol.NumAtoms();

    OBAtom* atom;

    for (auto i = atom_xyzs.begin(); i != atom_xyzs.end(); ++i) {

        atom = rec_mol.GetAtom(get_openbabel_index(*i, true));
        if (atom->GetX() < x + allowed_dist)
            if (atom->GetY() < y + allowed_dist)
                if (atom->GetZ() < z + allowed_dist)
                    if (atom->GetX() > x - allowed_dist)
                        if (atom->GetY() > y - allowed_dist)
                            if (atom->GetZ() > z - allowed_dist)
                                return true;
    }

    return false;
}

/*
//transforms score diff to maximize digits in b-factor
//field, and to raise small values by square-rooting
float cnn_visualization::transform_score_diff(float diff_val) {
    float temp = diff_val;

    if (temp < 0) {
        temp = 0 - std::sqrt(std::abs(temp));
    } else {
        temp = std::sqrt(temp);
    }

    temp = temp * 100;

    return temp;
}
*/

//returns concatenated xyz string of a given atom line
//for identifying atoms
std::string cnn_visualization::get_xyz(const std::string &line)
{
    std::string x = line.substr(31,7);
    std::string y = line.substr(39,7);
    std::string z = line.substr(48,6);
    boost::trim_left(x);
    boost::trim_left(y);
    boost::trim_left(z);
    std::string xyz = x + y + z;
    return xyz;
}

//removes whole residues at a time, and scores the resulting receptor
void cnn_visualization::remove_residues() {
    std::unordered_map<std::string, float> score_diffs;
    std::unordered_set<std::string> atoms_to_remove;
    std::unordered_map<std::string, std::unordered_set<std::string> > residues;

    std::string mol_string = rec_string;
    std::stringstream mol_stream(mol_string);
    std::string line;

    while (std::getline(mol_stream, line))
    {
        if (line.find("ATOM") < std::string::npos)
        {
            std::string res = line.substr(23, 4);
            residues[res].insert(get_xyz(line));
        }
    }

    int res_count = residues.size();
    int counter = 1;

    //iterate through residues
    //for (auto res = residues.begin(); res != residues.end(); ++res)
    for(const auto& res: residues)
    {
        if (!visopts.verbose)
        {
            std::cout << "Scoring residues: " << counter << '/' << res_count
                    << '\r' << std::flush;
            counter++;
        }

        mol_stream.clear();
        mol_stream.str(mol_string);

        atoms_to_remove = res.second;

        int counter = 0;
        for(auto atom : atoms_to_remove)
        {
            counter++;
        }

        bool remove = true;

        if (!visopts.skip_bound_check) {
            //make set for check_in_range test
            std::unordered_set<std::string> remove_set;
            for (auto i : atoms_to_remove) {
                remove_set.insert(i);
            }

            remove = check_in_range(remove_set);
        }

        if (remove) {
            std::string modified_mol_string = modify_pdbqt(atoms_to_remove, true);

            float score_val = score_modified_receptor(modified_mol_string);

            for (auto f : atoms_to_remove) {
                    float score_diff = original_score - score_val;
                    score_diffs[f] = score_diff;
            }
        }
        atoms_to_remove.clear();
    }

    write_scores(score_diffs, true, "masking");
    std::cout << '\n';
}

//checks all input indices for hydrogen neighbors, and appends them
void cnn_visualization::add_adjacent_hydrogens(
        std::unordered_set<int> &atoms_to_remove, bool isRec) {
    OBMol mol;

    if (isRec) {
        mol = rec_mol;
        mol.AddHydrogens();
    }

    else {
        mol = lig_mol;
        mol.AddHydrogens();
    }

    for (auto i : atoms_to_remove) {
        OBAtom* atom = mol.GetAtom(i);
        OBAtom* neighbor;
        for (OBAtomAtomIter neighbor(atom); neighbor; ++neighbor) {
            if (neighbor->GetAtomicNum() == 1) {
                atoms_to_remove.insert(neighbor->GetIdx());
            }
        }
    }
}

//mostly for debug convenience
void cnn_visualization::print_vector(const std::vector<int> &atoms_to_remove) {
    std::cout << "[" << atoms_to_remove[0];

    for (int i = 1; i < atoms_to_remove.size(); ++i) {
        std::cout << ", " << atoms_to_remove[i];
    }

    std::cout << "]";
}

//removes individual atoms, scores them, and returns the diffs
std::vector<float> cnn_visualization::remove_each_atom() {
    OBMol lig_mol_h = lig_mol;
    lig_mol_h.AddHydrogens(); //just in case hydrogen numbers don't add up

    std::vector<float> score_diffs(lig_mol_h.NumAtoms(), 0);
    std::stringstream lig_stream(lig_string);
    std::string line;

    std::string index_string;
    int atom_index;
    std::unordered_set<int> atoms_to_remove;
    float score_val;

    int counter = 1;
    int num_atoms = lig_mol.NumAtoms();

    while (std::getline(lig_stream, line)) {
        if (line.find("ATOM") < std::string::npos) {
            if (!visopts.verbose) {
                std::cout << "Scoring individual atoms: " << counter << '/'
                        << num_atoms << '\r' << std::flush;
                counter++;
            }

            std::string modified_mol_string;
            index_string = line.substr(6, 5);
            atom_index = std::stoi(index_string);

            if (lig_mol.GetAtom(atom_index)->GetAtomicNum() != 1) //don't remove hydrogens individually
                    {
                atoms_to_remove.insert(atom_index);
                add_adjacent_hydrogens(atoms_to_remove, false);

                modified_mol_string = modify_pdbqt(atoms_to_remove, false);
                score_val = score_modified_ligand(modified_mol_string);

                score_diffs[atom_index] = original_score - score_val;
            }

            atoms_to_remove.clear();
        }
    }

    if (visopts.verbose) {
        //print index:type for debugging
        for (auto i = lig_mol.BeginAtoms(); i != lig_mol.EndAtoms(); ++i) {
            std::cout << (*i)->GetIdx() << ": " << (*i)->GetType() << '\n';
        }
    }

    std::cout << '\n';
    return score_diffs;
}

//writes modified pdbqt strings to file, for debugging purposes
void cnn_visualization::output_modified_string(
        const std::string &modified_string,
        const std::vector<int> &atoms_removed, bool receptor) {
    static bool first = true;
    static int rec_counter = 0;
    static int lig_counter = 0;

    if (first) {
        ofstream original_rec_out;
        original_rec_out.open("unmodified_receptor.pdbqt");
        original_rec_out << rec_string;
        original_rec_out.close();

        ofstream original_lig_out;
        original_lig_out.open("unmodified_ligand.pdbqt");
        original_lig_out << lig_string;
        original_lig_out.close();

        first = false;
    }

    ofstream file_out;
    std::stringstream filename;

    if (receptor) {
        filename << "mod_receptor_" << atoms_removed[0];
    } else {
        filename << "mod_ligand_" << atoms_removed[0];
    }

    filename << ".pdbqt";

    file_out.open(filename.str());

    file_out << "REMARK: ATOMS REMOVED [" << atoms_removed[0];

    for (int i = 1; i < atoms_removed.size(); ++i) {
        file_out << ", " << atoms_removed[i];
    }

    file_out << "]\n";
    file_out << modified_string;
    file_out.close();
}

//writes sum of score differences for each atom along with original score to
//file for analysis
void cnn_visualization::write_additivity(const std::vector<float> &single_score_diffs,
        const std::vector<float> &frag_score_diffs) {
    float single_total = 0;
    float frag_total = 0;
    int num_atoms = lig_mol.NumAtoms();

    boost::filesystem::path local_name(visopts.ligand_name);
    boost::filesystem::path full_name = boost::filesystem::canonical(local_name);

    if (!visopts.frags_only) {
        for (int i = 1; i < single_score_diffs.size(); ++i) {
            if (i <= num_atoms) {
                if (lig_mol.GetAtom(i)->GetAtomicNum() != 1) //hydrogens will have score of 0
                        {
                    single_total += single_score_diffs[i];
                }
            }
        }
    }

    if (!visopts.atoms_only) {
        for (int i = 1; i < frag_score_diffs.size(); ++i) {
            if (i <= num_atoms) {
                if (lig_mol.GetAtom(i)->GetAtomicNum() != 1) //hydrogens will have score of 0
                        {
                    frag_total += frag_score_diffs[i];
                }
            }
        }
    }

    std::ofstream out_file;
    out_file.open(visopts.additivity, std::ios_base::app);

    if (visopts.verbose) {
        std::cout << "ORIGINAL SCORE: " << original_score << '\n';

        if (!visopts.frags_only) {
            std::cout << "SUM OF SINGLE REMOVALS: " << single_total << '\n';
        }
        if (!visopts.atoms_only) {
            std::cout << "SUM OF FRAGMENT REMOVALS: " << frag_total << '\n';
        }
    }

    out_file << full_name.string() << " " << original_score << " "
            << single_total << " " << frag_total << "\n";

    out_file.close();

}

//wrapper for fragment and individual removals
void cnn_visualization::remove_ligand_atoms() {
    std::vector<float> individual_score_diffs;
    std::vector<float> frag_score_diffs;
    if (visopts.atoms_only) {
        individual_score_diffs = remove_each_atom();
        write_scores(individual_score_diffs, false, "masking");
    }

    else if (visopts.frags_only) {
        frag_score_diffs = remove_fragments(6);
        write_scores(frag_score_diffs, false, "masking");
    }

    else {
        individual_score_diffs = remove_each_atom();
        frag_score_diffs = remove_fragments(6);

        std::vector<float> both_score_diffs(individual_score_diffs.size(),
                0.00);

        //average individual and fragment diffs
        for (int i = 0; i != individual_score_diffs.size(); ++i) {
            float avg = (individual_score_diffs[i] + frag_score_diffs[i]) / 2;
            both_score_diffs[i] = avg;
        }

        write_scores(both_score_diffs, false, "masking");

    }

    if (visopts.additivity.length() > 0) {
        write_additivity(individual_score_diffs, frag_score_diffs);
    }

}


std::string cnn_visualization::get_xyz_from_index(int index, bool rec) {
    static bool first = true;
    static std::map<int, std::string> rec_indices;
    static std::map<int, std::string> lig_indices;

    //fill map on first run
    if (first)
    {
        std::stringstream mol_stream;
        std::string line;
        mol_stream = std::stringstream(lig_string);
        while (std::getline(mol_stream, line))
        {
            if (line.find("ATOM") < std::string::npos ||
                 line.find("HETATM") < std::string::npos)
            {
                std::string line_xyz = get_xyz(line);

                std::string index_string = line.substr(7, 5);
                int atom_index = std::stoi(index_string);

                lig_indices[atom_index] = line_xyz;
            }
        }

        mol_stream = std::stringstream(rec_string);
        while (std::getline(mol_stream, line))
        {
            if (line.find("ATOM") < std::string::npos ||
                 line.find("HETATM") < std::string::npos)
            {
                std::string line_xyz = get_xyz(line);

                std::string index_string = line.substr(7, 5);
                int atom_index = std::stoi(index_string);

                rec_indices[atom_index] = line_xyz;
            }
        }
        first = false;
    }

    if(rec)
    {
        return rec_indices[index];
    }
    else
    {
        return lig_indices[index];
    }
}
//returns openbabel index of atom with supplied xyz coordinate
int cnn_visualization::get_openbabel_index(const std::string &xyz, bool rec) {
    static bool first = true;
    static std::map<std::string, int> rec_indices;
    static std::map<std::string, int> lig_indices;

    //fill map on first run
    if (first) 
    {
        std::stringstream mol_stream;
        std::string line;
        mol_stream = std::stringstream(lig_string);
        while (std::getline(mol_stream, line))
        {
            if (line.find("ATOM") < std::string::npos ||
                 line.find("HETATM") < std::string::npos)
            {
                std::string line_xyz = get_xyz(line);

                std::string index_string = line.substr(7, 5);
                int atom_index = std::stoi(index_string);

                lig_indices[line_xyz] = atom_index;
            }
        }

        mol_stream = std::stringstream(rec_string);
        while (std::getline(mol_stream, line))
        {
            if (line.find("ATOM") < std::string::npos ||
                 line.find("HETATM") < std::string::npos)
            {
                std::string line_xyz = get_xyz(line);

                std::string index_string = line.substr(7, 5);
                int atom_index = std::stoi(index_string);

                rec_indices[line_xyz] = atom_index;
            }
        }
        first = false;
    }

    if(rec)
    {
        return rec_indices[xyz];
    }
    else
    {
        return lig_indices[xyz];
    }
}

//returns average score difference for each index across all fragments
std::vector<float> cnn_visualization::remove_fragments(int size) {
    OBConversion conv;

    OBMol lig_mol_h = lig_mol;
    lig_mol_h.AddHydrogens(); //just in case hydrogen numbers don't add up

    std::vector<float> score_diffs(lig_mol_h.NumAtoms() + 1, 0.00);
    std::vector<int> score_counts(lig_mol_h.NumAtoms() + 1, 0); //used to calculate average of scores across all fragments

    //PDB has parsing issues with RDKit
    conv.SetOutFormat("MOL");
    std::stringstream MOL_stream(conv.WriteString(&lig_mol));

    unsigned int line = 0;

    RDKit::RWMol rdkit_mol(
            *(RDKit::MolDataStreamToMol(MOL_stream, line, false, true, false))); //removeHs = true
    RDKit::MolOps::removeHs(rdkit_mol, false, false, false); //hydrogens will be added by add_adjacent_hydrogens later

    if (visopts.verbose) {
        //print all bonds in rdkit_mol, to check against fragments
        for (auto i = rdkit_mol.beginBonds(); i != rdkit_mol.endBonds(); ++i) {
            int first = (*i)->getBeginAtomIdx() + 1;
            int second = (*i)->getEndAtomIdx() + 1;
        }
    }

    std::unordered_set<int> atoms_to_remove;

    //map of path length: list of paths
    RDKit::INT_PATH_LIST_MAP paths = RDKit::findAllSubgraphsOfLengthsMtoN(
            rdkit_mol, 1, size);

    RDKit::Conformer conf = rdkit_mol.getConformer();

    int path_count = 0;

    //count number of fragments for progress output
    for (auto path = paths.begin(); path != paths.end(); ++path) //iterate through path lengths
            {
        std::list<std::vector<int>> list_of_lists = std::get < 1 > (*path);
        for (auto bonds = list_of_lists.begin(); bonds != list_of_lists.end();
                ++bonds) //iterate through paths of a given length
                {
            path_count++;
        }
    }

    int counter = 1; //stores current fragment number for progress output

    for (auto path = paths.begin(); path != paths.end(); ++path) //iterate through path lengths
            {
        std::list<std::vector<int>> list_of_lists = std::get < 1 > (*path);

        for (auto bonds = list_of_lists.begin(); bonds != list_of_lists.end();
                ++bonds) //iterate through paths of a given length
                {
            std::vector<int> bond_list = *bonds;

            if (!visopts.verbose) {
                std::cout << "Scoring fragments: " << counter << '/'
                        << path_count << '\r' << std::flush;
                counter++;
            }

            for (int i = 0; i < bond_list.size(); ++i) //iterate through bonds in path
                    {
                RDKit::Bond bond = *(rdkit_mol.getBondWithIdx(bond_list[i]));
                int first = bond.getBeginAtomIdx();
                int second = bond.getEndAtomIdx();

                atoms_to_remove.insert(first + 1);
                atoms_to_remove.insert(second + 1);
            }

            int size_without_hydrogens = atoms_to_remove.size();
            add_adjacent_hydrogens(atoms_to_remove, false);

            std::string modified_ligand = modify_pdbqt(atoms_to_remove, false);
            float score = score_modified_ligand(modified_ligand);

            for (auto index: atoms_to_remove) {
                score_diffs[index] += (original_score - score)
                        / size_without_hydrogens; //give each atom in removal equal portion of score difference
                score_counts[index] += 1;
            }

            atoms_to_remove.clear(); //clear for next group of atoms to be removed
        }
    }

    std::vector<float> avg_score_diffs(lig_mol.NumAtoms() + 1, 0.00);
    for (auto i = rdkit_mol.beginAtoms(); i != rdkit_mol.endAtoms(); ++i) {
        int r_index = (*i)->getIdx();
        int index = r_index + 1;
        if (score_counts[index] > 0) {
            avg_score_diffs[index] = score_diffs[index] / score_counts[index];
            if (visopts.verbose) {
                std::cout << "Symbol: " << (*i)->getSymbol() << '\n';
                double x = (rdkit_mol.getConformer().getAtomPos(r_index)).x;
                std::cout << "X: " << x << '\n';
                std::cout << "RDKit Index: " << r_index << '\n';
                std::cout << "Corrected Index: " << index << '\n';
                std::cout << "Agg. Score Diff: " << score_diffs[index] << '\n';
                std::cout << "Score count: " << score_counts[index] << '\n';
                std::cout << "Avg. Score Diff: " << avg_score_diffs[index]
                        << '\n';
                std::cout << "===============" << '\n';
            }
        }

        else {
            avg_score_diffs[index] = 0.00;
        }
    }

    std::cout << '\n';
    return avg_score_diffs;
}

