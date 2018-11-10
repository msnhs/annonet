/*
    This example shows how to train a semantic segmentation net using images
    annotated in the "anno" program (see https://github.com/reunanen/anno).

    Instructions:
    1. Use anno to label some data.
    2. Build the annonet_train program.
    3. Run:
       ./annonet_train /path/to/anno/data
    4. Wait while the network is being trained.
    5. Build the annonet_infer example program.
    6. Run:
       ./annonet_infer /path/to/anno/data
*/

#include "annonet.h"
#include "annonet_infer.h"

#include "cxxopts/include/cxxopts.hpp"
#include <iostream>
#include <dlib/data_io.h>
#include <dlib/gui_widgets.h>
#include <dlib/image_saver/save_png.h>

using namespace std;
using namespace dlib;

// ----------------------------------------------------------------------------------------

struct class_specific_value_type
{
    uint16_t class_index = dlib::loss_multiclass_log_per_pixel_::label_to_ignore;
    double value = 0.0;
};

class_specific_value_type parse_class_specific_value(const std::string& string_from_command_line)
{
    const auto colon_pos = string_from_command_line.find(':');
    if (colon_pos == std::string::npos || colon_pos < 1 || colon_pos >= string_from_command_line.length() - 1) {
        throw std::runtime_error("The gains must be supplied in the format index:gain (e.g., 1:-0.5)");
    }
    class_specific_value_type class_specific_value;
    class_specific_value.class_index = std::stoul(string_from_command_line.substr(0, colon_pos));
    class_specific_value.value = std::stod(string_from_command_line.substr(colon_pos + 1));
    return class_specific_value;
}

std::vector<double> parse_class_specific_values(const std::vector<std::string>& strings_from_command_line, uint16_t class_count)
{
    std::vector<double> class_specific_values(class_count, 0.0);

    for (const auto string_from_command_line : strings_from_command_line) {
        const auto class_specific_value = parse_class_specific_value(string_from_command_line);
        if (class_specific_value.class_index >= class_count) {
            std::ostringstream error;
            error << "Can't define class-specific value for index " << class_specific_value.class_index << " when there are only " << class_count << " classes";
            throw std::runtime_error(error.str());
        }
        class_specific_values[class_specific_value.class_index] = class_specific_value.value;
    }

    return class_specific_values;
}

// ----------------------------------------------------------------------------------------

inline rgb_alpha_pixel index_label_to_rgba_label(uint16_t index_label, const std::vector<AnnoClass>& anno_classes)
{
    const AnnoClass& anno_class = anno_classes[index_label];
    assert(anno_class.index == index_label);
    return anno_class.rgba_label;
}

void index_label_image_to_rgba_label_image(const matrix<uint16_t>& index_label_image, matrix<rgb_alpha_pixel>& rgba_label_image, const std::vector<AnnoClass>& anno_classes)
{
    const long nr = index_label_image.nr();
    const long nc = index_label_image.nc();

    rgba_label_image.set_size(nr, nc);

    for (long r = 0; r < nr; ++r) {
        for (long c = 0; c < nc; ++c) {
            rgba_label_image(r, c) = index_label_to_rgba_label(index_label_image(r, c), anno_classes);
        }
    }
}

// ----------------------------------------------------------------------------------------

struct result_image_type {
    std::string filename;
    int original_width = 0;
    int original_height = 0;
    matrix<uint8_t> probability_image;
};

int main(int argc, char** argv) try
{
    if (argc == 1)
    {
        cout << "You call this program like this: " << endl;
        cout << "./annonet_infer /path/to/image/data" << endl;
        cout << endl;
        cout << "You will also need a trained 'annonet.dnn' file. " << endl;
        cout << endl;
        return 1;
    }

    cxxopts::Options options("annonet_infer", "Do inference using trained semantic-segmentation networks");

    std::ostringstream hardware_concurrency;
    hardware_concurrency << std::thread::hardware_concurrency();

#ifdef DLIB_USE_CUDA
    const std::string default_max_tile_width = "512";
    const std::string default_max_tile_height = "512";
#else
    // in CPU-only mode, we can handle larger tiles
    const std::string default_max_tile_width = "4096";
    const std::string default_max_tile_height = "4096";
#endif

    options.add_options()
        ("i,input-directory", "Input image directory", cxxopts::value<std::string>())
        ("g,gain", "Supply a class-specific gain, for example: 1:-0.5", cxxopts::value<std::vector<std::string>>())
        ("w,tile-max-width", "Set max tile width", cxxopts::value<int>()->default_value(default_max_tile_width))
        ("h,tile-max-height", "Set max tile height", cxxopts::value<int>()->default_value(default_max_tile_height))
        ("full-image-reader-thread-count", "Set the number of full-image reader threads", cxxopts::value<int>()->default_value(hardware_concurrency.str()))
        ("result-image-writer-thread-count", "Set the number of result-image writer threads", cxxopts::value<int>()->default_value(hardware_concurrency.str()))
        ;

    try {
        options.parse_positional("input-directory");
        options.parse(argc, argv);

        cxxopts::check_required(options, { "input-directory" });

        std::cout << "Input directory = " << options["input-directory"].as<std::string>() << std::endl;
    }
    catch (std::exception& e) {
        cerr << e.what() << std::endl;
        cerr << std::endl;
        cerr << options.help() << std::endl;
        return 2;
    }

    double downscaling_factor = 1.0;
    std::string serialized_runtime_net;
    std::string anno_classes_json;
    deserialize("annonet.dnn") >> anno_classes_json >> downscaling_factor >> serialized_runtime_net;

    std::cout << "Deserializing annonet, downscaling factor = " << downscaling_factor << std::endl;

    NetPimpl::RuntimeNet net;
    net.Deserialize(std::istringstream(serialized_runtime_net));

    const std::vector<AnnoClass> anno_classes = parse_anno_classes(anno_classes_json);

    DLIB_CASSERT(anno_classes.size() >= 2);

    const std::vector<double> gains = parse_class_specific_values(options["gain"].as<std::vector<std::string>>(), anno_classes.size());

    assert(gains.size() == anno_classes.size());

    std::cout << "Using gains:";
    for (size_t class_index = 0, end = gains.size(); class_index < end; ++class_index) {
        std::cout << " " << class_index << ":" << gains[class_index];
    }
    std::cout << std::endl;

    set_low_priority();

    annonet_infer_temp temp;
    matrix<uint16_t> index_label_tile_resized;

    auto files = find_image_files(options["input-directory"].as<std::string>(), false);

    dlib::pipe<image_filenames> full_image_read_requests(files.size());
    for (const image_filenames& file : files) {
        full_image_read_requests.enqueue(image_filenames(file));
    }

    const int full_image_reader_count = std::max(1, options["full-image-reader-thread-count"].as<int>());
    const int result_image_writer_count = std::max(1, options["result-image-writer-thread-count"].as<int>());

    dlib::pipe<sample> full_image_read_results(full_image_reader_count);

    std::vector<std::thread> full_image_readers;

    for (unsigned int i = 0; i < full_image_reader_count; ++i) {
        full_image_readers.push_back(std::thread([&]() {
            image_filenames image_filenames;
            while (full_image_read_requests.dequeue(image_filenames)) {
                full_image_read_results.enqueue(read_sample(image_filenames, anno_classes, false, downscaling_factor));
            }
        }));
    }

    dlib::pipe<result_image_type> result_image_write_requests(result_image_writer_count);
    dlib::pipe<bool> result_image_write_results(files.size());

    std::vector<std::thread> result_image_writers;

    for (unsigned int i = 0; i < result_image_writer_count; ++i) {
        result_image_writers.push_back(std::thread([&]() {
            result_image_type result_image;
            dlib::matrix<uint8_t> resized_result_image;
            while (result_image_write_requests.dequeue(result_image)) {
                resized_result_image.set_size(result_image.original_height, result_image.original_width);
                dlib::resize_image(result_image.probability_image, resized_result_image);
                save_png(resized_result_image, result_image.filename);
                result_image_write_results.enqueue(true);
            }
        }));
    }

    const int min_input_dimension = NetPimpl::TrainingNet::GetRequiredInputDimension();

    tiling::parameters tiling_parameters;
    tiling_parameters.max_tile_width = options["tile-max-width"].as<int>();
    tiling_parameters.max_tile_height = options["tile-max-height"].as<int>();
    tiling_parameters.overlap_x = min_input_dimension;
    tiling_parameters.overlap_y = min_input_dimension;

    DLIB_CASSERT(tiling_parameters.max_tile_width >= min_input_dimension);
    DLIB_CASSERT(tiling_parameters.max_tile_height >= min_input_dimension);

    const auto t0 = std::chrono::steady_clock::now();

    for (size_t i = 0, end = files.size(); i < end; ++i)
    {
        std::cout << "\rProcessing image " << (i + 1) << " of " << end << "...";

        sample sample;
        result_image_type result_image;

        full_image_read_results.dequeue(sample);

        if (!sample.error.empty()) {
            throw std::runtime_error(sample.error);
        }

        const auto& input_image = sample.input_image;

        result_image.filename = sample.image_filenames.image_filename + "_probability_map.png";
        result_image.probability_image.set_size(input_image.nr(), input_image.nc());
        result_image.original_width = sample.original_width;
        result_image.original_height = sample.original_height;

        annonet_infer(net, sample.input_image, result_image.probability_image, gains, tiling_parameters, temp);

        result_image_write_requests.enqueue(result_image);
    }

    const auto t1 = std::chrono::steady_clock::now();

    std::cout << "\nAll " << files.size() << " images processed in "
        << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0 << " seconds!" << std::endl;

    for (size_t i = 0, end = files.size(); i < end; ++i) {
        bool ok;
        result_image_write_results.dequeue(ok);
    }

    std::cout << "All result images written!" << std::endl;

    full_image_read_requests.disable();
    result_image_write_requests.disable();

    for (std::thread& image_reader : full_image_readers) {
        image_reader.join();
    }
    for (std::thread& image_writer : result_image_writers) {
        image_writer.join();
    }
}
catch(std::exception& e)
{
    cout << e.what() << endl;
    return 1;
}