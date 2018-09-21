#include "Config.hpp"
#include "Geometry.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMesh.hpp"
#include "Format/3mf.hpp"
#include "libslic3r.h"
#include "Utils.hpp"
#include <cstdio>
#include <string>
#include <cstring>
#include <iostream>
#include <math.h>
#include <boost/filesystem.hpp>
#include <boost/nowide/args.hpp>
#include <boost/nowide/iostream.hpp>

#ifdef USE_WX
//    #include "GUI/GUI.hpp"
#endif
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

using namespace Slic3r;

/// utility function for displaying CLI usage
void printUsage();

using namespace Slic3r;

int main(int argc, char **argv)
{
    // Convert arguments to UTF-8 (needed on Windows). argv then points to memory owned by a.
    //FIXME On Windows, we want to receive the arguments as 16bit characters!
    boost::nowide::args a(argc, argv);

    // parse all command line options into a DynamicConfig
    DynamicPrintAndCLIConfig config;
    t_config_option_keys input_files;
    // if any option is unsupported, print usage and abort immediately
    if (! config.read_cli(argc, argv, &input_files)) {
        printUsage();
        return 0;
    }

    boost::filesystem::path path_to_binary = boost::filesystem::system_complete(argv[0]);
    boost::filesystem::path path_resources = path_to_binary.parent_path();
    // Path from the Slic3r binary to its resources.
    path_resources /= (path_to_binary.stem().string() == "slic3r-gui") ? 
        // Running from the build directory:
        "../../resources" :
        // Running from an installation directory:
#if APPLE
        // The application is packed in the .dmg archive as 'Slic3r.app/Contents/MacOS/Slic3r'
        // The resources are packed to 'Slic3r.app/Contents/Resources'
        "../Resources"
#else
    #if WIN32
        // The application is packed in the .zip archive in the root,
        // The resources are packed to 'resources'
        // Path from Slic3r binary to resources:
        "resources"
    #else
        // The application is packed in the .tar.bz archive (or in AppImage) as 'bin/slic3r',
        // The resources are packed to 'resources'
        // Path from Slic3r binary to resources:
        "../resources"
    #endif
#endif
        ;
    set_resources_dir(path_resources.string());
    set_var_dir((path_resources / "icons").string());
    set_local_dir((path_resources / "localization").string());

    // apply command line options to a more handy CLIConfig
    CLIConfig cli_config;
    cli_config.apply(config, true);
    set_local_dir(cli_config.datadir.value);

    DynamicPrintConfig print_config;

    if ((argc == 1 || cli_config.gui.value) && ! cli_config.no_gui.value && ! cli_config.help.value && cli_config.save.value.empty()) {
#if 1
        GUI::GUI_App *gui = new GUI::GUI_App();
        GUI::GUI_App::SetInstance(gui);
        wxEntry(argc, argv);
#else
        std::cout << "GUI support has not been built." << "\n";
#endif
    }
    // load config files supplied via --load
    for (const std::string &file : cli_config.load.values) {
        if (!boost::filesystem::exists(file)) {
            boost::nowide::cout << "No such file: " << file << std::endl;
            exit(1);
        }
        DynamicPrintConfig c;
        try {
            c.load(file);
        } catch (std::exception &e) {
            boost::nowide::cout << "Error while reading config file: " << e.what() << std::endl;
            exit(1);
        }
        c.normalize();
        print_config.apply(c);
    }
    
    // apply command line options to a more specific DynamicPrintConfig which provides normalize()
    // (command line options override --load files)
    print_config.apply(config, true);
    print_config.normalize();
    
    // write config if requested
    if (! cli_config.save.value.empty())
        print_config.save(cli_config.save.value);

    // read input file(s) if any
    std::vector<Model> models;
    for (const t_config_option_key &file : input_files) {
        if (! boost::filesystem::exists(file)) {
            boost::nowide::cerr << "No such file: " << file << std::endl;
            exit(1);
        }
        
        Model model;
        try {
            model = Model::read_from_file(file);
        } catch (std::exception &e) {
            boost::nowide::cerr << file << ": " << e.what() << std::endl;
            exit(1);
        }
        
        if (model.objects.empty()) {
            boost::nowide::cerr << "Error: file is empty: " << file << std::endl;
            continue;
        }
        
        model.add_default_instances();
        
        // apply command line transform options
        for (ModelObject* o : model.objects) {
/*
            if (cli_config.scale_to_fit.is_positive_volume())
                o->scale_to_fit(cli_config.scale_to_fit.value);
*/
            // TODO: honor option order?
            o->scale(cli_config.scale.value);
            o->rotate(Geometry::deg2rad(cli_config.rotate_x.value), X);
            o->rotate(Geometry::deg2rad(cli_config.rotate_y.value), Y);
            o->rotate(Geometry::deg2rad(cli_config.rotate.value), Z);
        }
        
        // TODO: handle --merge
        models.push_back(model);
    }
    if (cli_config.help) {
        printUsage();
        return 0;
    }
    
    for (Model &model : models) {
        if (cli_config.info) {
            // --info works on unrepaired model
            model.print_info();
        } else if (cli_config.export_3mf) {
            std::string outfile = cli_config.output.value;
            if (outfile.empty()) outfile = model.objects.front()->input_file;
            // Check if the file is already a 3mf.
            if(outfile.substr(outfile.find_last_of('.'), outfile.length()) == ".3mf")
                outfile = outfile.substr(0, outfile.find_last_of('.')) + "_2" + ".3mf";
            else
                // Remove the previous extension and add .3mf extention.
                outfile = outfile.substr(0, outfile.find_last_of('.')) + ".3mf";
            store_3mf(outfile.c_str(), &model, nullptr, false);
            boost::nowide::cout << "File file exported to " << outfile << std::endl;
        } else if (cli_config.cut > 0) {
            model.repair();
            model.translate(0, 0, - model.bounding_box().min(2));
            if (! model.objects.empty()) {
                Model out;
                model.objects.front()->cut(cli_config.cut, &out);
                ModelObject &upper = *out.objects[0];
                ModelObject &lower = *out.objects[1];
                // Use the input name and trim off the extension.
                std::string outfile = cli_config.output.value;
                if (outfile.empty()) 
                    outfile = model.objects.front()->input_file;
                outfile = outfile.substr(0, outfile.find_last_of('.'));
                std::cerr << outfile << "\n";
                if (upper.facets_count() > 0)
                    upper.mesh().write_binary((outfile + "_upper.stl").c_str());
                if (lower.facets_count() > 0)
                    lower.mesh().write_binary((outfile + "_lower.stl").c_str());
            }
        } else if (cli_config.slice) {
            std::string outfile = cli_config.output.value;
            Print print;
            model.arrange_objects(print.config().min_object_distance());
            model.center_instances_around_point(cli_config.center);
            if (outfile.empty()) outfile = model.objects.front()->input_file + ".gcode";
            print.apply_config(print_config);
            for (auto* mo : model.objects) {
                print.auto_assign_extruders(mo);
                print.add_model_object(mo);
            }
            print.validate();
            print.export_gcode(outfile, nullptr);
        } else {
            boost::nowide::cerr << "error: command not supported" << std::endl;
            return 1;
        }
    }
    
    return 0;
}

void printUsage()
{
    std::cout << "Slic3r " << SLIC3R_VERSION << " is a STL-to-GCODE translator for RepRap 3D printers" << "\n"
              << "written by Alessandro Ranellucci <aar@cpan.org> - http://slic3r.org/ - https://github.com/slic3r/Slic3r" << "\n"
//              << "Git Version " << BUILD_COMMIT << "\n\n"
              << "Usage: ./slic3r [ OPTIONS ] [ file.stl ] [ file2.stl ] ..." << "\n";
    // CLI Options
    std::cout << "** CLI OPTIONS **\n";
    print_cli_options(boost::nowide::cout);
    std::cout << "****\n";
        // Print options
        std::cout << "** PRINT OPTIONS **\n";
    print_print_options(boost::nowide::cout);
    std::cout << "****\n";
}
