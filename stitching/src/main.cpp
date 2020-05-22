/*
* stitching - command line tool for whole-body image stitching.
*
* Copyright 2016 Ben Glocker <b.glocker@imperial.ac.uk>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "itkio.h"
#include "miaImage.h"
#include "miaImageProcessing.h"

#include <iostream>
#include <vector>
#include <chrono>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

using namespace mia;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

template <typename T>
void strings_to_values(const std::vector<std::string>& string_seq, std::vector<T>& values)
{
  for(std::vector<std::string>::const_iterator it = string_seq.begin(); it != string_seq.end(); ++it)
  {
    std::stringstream ss(*it);
    std::copy(std::istream_iterator<T>(ss), std::istream_iterator<T>(), back_inserter(values));
  }
}

int main(int argc, char* argv[])
{
  std::vector<std::string> sfiles;
  std::string filename_out;
  int margin = 0;
  bool average_overlap = false;

  try
  {
    // Declare the supported options.
    po::options_description options("options");
    options.add_options()
    ("help,h", "produce help message")
    ("images,i", po::value<std::vector<std::string>>(&sfiles)->multitoken(), "filenames of images")
    ("output,o", po::value<std::string>(&filename_out), "filename of output image")
    ("margin,m", po::value<int>(&margin), "image margin that is ignored when stitching")
    ("averaging,a", po::bool_switch(&average_overlap)->default_value(false), "enable averaging in overlap areas")
    ;

    po::variables_map vm;

    po::store(po::parse_command_line(argc, argv, options), vm);
    po::notify(vm);

    if (vm.count("help") || vm.size() == 0)
    {
      std::cout << options << std::endl;
      return 0;
    }
  }
  catch(std::exception& e)
  {
    std::cout << e.what() << std::endl;
    return 1;
  }

  // convert string-based parameters to values
  std::vector<std::string> files;
  strings_to_values(sfiles, files);

  if (files.size() > 1)
  {
    namespace ch = std::chrono;
    auto start = ch::high_resolution_clock::now();
    std::cout << "stitching image...";

    float unique_value = -123456789.0;
    auto loaded = itkio::load(files[0]);

    // determine physical extent of stitched volume
    auto image0 = subimage(loaded, 0, 0, margin, loaded.sizeX(), loaded.sizeY(), loaded.sizeZ()-2*margin).clone();
    auto image0_min_extent = image0.origin()[2];
    auto image0_max_extent = image0.origin()[2] + image0.sizeZ() * image0.spacing()[2];
    auto min_extent = image0_min_extent;
    auto max_extent = image0_max_extent;
    std::vector<Image> images;
    for (int i = 1; i < files.size(); i++)
    {
      auto temp = itkio::load(files[i]);
      auto image = subimage(temp, 0, 0, margin, temp.sizeX(), temp.sizeY(), temp.sizeZ()-2*margin).clone();
      images.push_back(image);
      if (image.origin()[2] < min_extent)
      {
        min_extent = image.origin()[2];
      }
      if (image.origin()[2] + image.sizeZ() * image.spacing()[2] > max_extent)
      {
        max_extent = image.origin()[2] + image.sizeZ() * image.spacing()[2];
      }
    }

    // generate stitched volume and fill in first image
    auto pad_min_z = static_cast<int>((image0_min_extent - min_extent) / image0.spacing()[2] + 1);
    auto pad_max_z = static_cast<int>((max_extent - image0_max_extent) / image0.spacing()[2] + 1);
    auto target = pad(image0, 0, 0, 0, 0, pad_min_z, pad_max_z, unique_value);
    auto counts = target.clone();

    //find valid image values
    threshold(target, counts, unique_value, unique_value);
    invert_binary(counts, counts);
    mul(target, counts, target);

    // iterate over remaining images and add to stitched volume
    auto binary = target.clone();
    auto empty = target.clone();
    for (int i = 0; i < images.size(); i++)
    {
      threshold(counts, empty, 0, 0);
      auto trg = target.clone();
      resample(images[i], trg, mia::LINEAR, unique_value);
      threshold(trg, binary, unique_value, unique_value);
      invert_binary(binary, binary);

      //take only value for empty voxels, otherwise average values in overlap areas
      if (!average_overlap) mul(empty, binary, binary);

      mul(trg, binary, trg);
      add(trg, target, target);
      add(binary, counts, counts);
    }

    //remove extra empty slices introduced to rounding of pad values
    int central_x = counts.sizeX() / 2;
    int central_y = counts.sizeY() / 2;
    int off_z_min = 0;
    while (counts(central_x, central_y, off_z_min) == 0 && off_z_min < counts.sizeZ() - 1)
    {
      off_z_min++;
    }
    int off_z_max = counts.sizeZ();
    while (counts(central_x, central_y, off_z_max - 1) == 0 && off_z_max > 0)
    {
      off_z_max--;
    }

    threshold(counts, binary, 0, 0);
    add(binary, counts, counts);
    div(target, counts, target);

    target = subimage(target, 0, 0, off_z_min, target.sizeX(), target.sizeY(), off_z_max - off_z_min).clone();

    target.dataType(mia::FLOAT);
    itkio::save(target, filename_out);

    auto stop = ch::high_resolution_clock::now();
    std::cout << "done. took " << ch::duration_cast< ch::milliseconds >(stop-start).count() << " ms" << std::endl;
  }
}
