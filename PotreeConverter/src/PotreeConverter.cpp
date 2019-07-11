

#include <experimental/filesystem>

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include "BINPointReader.hpp"
#include "LASPointReader.h"
#include "PTXPointReader.h"
#include "PlyPointReader.h"
#include "PotreeConverter.h"
#include "PotreeException.h"
#include "PotreeWriter.h"
#include "Transformation.h"
#include "XYZPointReader.hpp"
#include "stuff.h"
#include "ThroughputCounter.hpp"

#include <math.h>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

#include "TileSetWriter.h"

using rapidjson::Document;
using rapidjson::PrettyWriter;
using rapidjson::StringBuffer;
using rapidjson::Value;
using rapidjson::Writer;

using std::find;
using std::fstream;
using std::map;
using std::string;
using std::stringstream;
using std::vector;
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

namespace fs = std::experimental::filesystem;

namespace Potree {

constexpr auto PROCESS_COUNT = 1'000'000;

static std::unique_ptr<SRSTransformHelper> get_transformation_helper(
    const std::optional<std::string> &sourceProjection) {
  if (!sourceProjection) {
    std::cout << "Source projection not specified, skipping point "
                 "transformation..."
              << std::endl;
    return std::make_unique<IdentityTransform>();
  }

  if (std::strcmp(sourceProjection->c_str(),
                  "+proj=longlat +datum=WGS84 +no_defs") == 0) {
    std::cout << "Source projection is already WGS84, skipping point "
                 "transformation..."
              << std::endl;
    return std::make_unique<IdentityTransform>();
  }

  try {
    return std::make_unique<Proj4Transform>(*sourceProjection);
  } catch (const std::runtime_error &err) {
    std::cerr << "Error while setting up coordinate transformation:\n"
              << err.what() << std::endl;
    std::cerr << "Skipping point transformation..." << std::endl;
  }
  return std::make_unique<IdentityTransform>();
}

/// <summary>
/// Verify that output directory is valid
/// </summary>
static void verifyWorkDir(const std::string &workDir, StoreOption storeOption) {
  if (fs::exists(workDir)) {
    const auto rootFile = workDir + "/r.json";
    if (fs::exists(rootFile) && storeOption == StoreOption::ABORT_IF_EXISTS) {
      throw std::runtime_error{
          "Output directory is not empty. Specify --overwrite if you want to "
          "overwrite the contents of the output folder!"};
    }

    if (storeOption == StoreOption::INCREMENTAL) {
        std::cout << "Appending to existing output directory..." << std::endl;
        return;
    }

    std::cout << "Output directory not empty, removing existing files..."
              << std::endl;
    for (auto &entry : fs::directory_iterator{workDir}) {
      fs::remove_all(entry);
    }
  } else {
    std::cout << "Output directory does not exist, creating it..." << std::endl;
    fs::create_directories(workDir);
  }
}

PointReader *PotreeConverter::createPointReader(
    const string& path, const PointAttributes &pointAttributes) const {
  PointReader *reader = NULL;
  if (iEndsWith(path, ".las") || iEndsWith(path, ".laz")) {
    reader = new LASPointReader(path, pointAttributes);
  } else if (iEndsWith(path, ".ptx")) {
    reader = new PTXPointReader(path);
  } else if (iEndsWith(path, ".ply")) {
    reader = new PlyPointReader(path);
  } else if (iEndsWith(path, ".xyz") || iEndsWith(path, ".txt")) {
    reader = new XYZPointReader(path, format, colorRange, intensityRange);
  } else if (iEndsWith(path, ".pts")) {
    vector<double> intensityRange;

    if (this->intensityRange.size() == 0) {
      intensityRange.push_back(-2048);
      intensityRange.push_back(+2047);
    }

    reader = new XYZPointReader(path, format, colorRange, intensityRange);
  } else if (iEndsWith(path, ".bin")) {
    reader = new BINPointReader(path, aabb, scale, pointAttributes);
  }

  return reader;
}

PotreeConverter::PotreeConverter(string executablePath, string workDir,
                                 vector<string> sources) :
  _ui(&_ui_state) {
  this->executablePath = executablePath;
  this->workDir = workDir;
  this->sources = sources;
}

void PotreeConverter::prepare() {
  verifyWorkDir(workDir, storeOption);

  // if sources contains directories, use files inside the directory instead
  vector<string> sourceFiles;
  for (const auto &source : sources) {
    fs::path pSource(source);
    if (fs::is_directory(pSource)) {
      fs::directory_iterator it(pSource);
      for (; it != fs::directory_iterator(); it++) {
        fs::path pDirectoryEntry = it->path();
        if (fs::is_regular_file(pDirectoryEntry)) {
          string filepath = pDirectoryEntry.string();
          if (iEndsWith(filepath, ".las") || iEndsWith(filepath, ".laz") ||
              iEndsWith(filepath, ".xyz") || iEndsWith(filepath, ".pts") ||
              iEndsWith(filepath, ".ptx") || iEndsWith(filepath, ".ply")) {
            sourceFiles.push_back(filepath);
          }
        }
      }
    } else if (fs::is_regular_file(pSource)) {
      sourceFiles.push_back(source);
    } else {
      std::cout << "Can't open input file \"" << source << "\"" << std::endl;
    }
  } 

  //Remove input files that don't exist and notify user
  sourceFiles.erase(std::remove_if(sourceFiles.begin(), sourceFiles.end(), [](const auto& path) { 
    if(fs::exists(path)) return false;
    std::cout << "Can't open input file \"" << path << "\"" << std::endl;
    return true;
  }), sourceFiles.end());

  this->sources = sourceFiles;

  pointAttributes.add(attributes::POSITION_CARTESIAN);
  for (const auto &attribute : outputAttributes) {
    if (attribute == "RGB") {
      pointAttributes.add(attributes::COLOR_PACKED);
    } else if (attribute == "RGB_FROM_INTENSITY") {
      pointAttributes.add(attributes::COLOR_FROM_INTENSITY);
    } else if (attribute == "INTENSITY") {
      pointAttributes.add(attributes::INTENSITY);
    } else if (attribute == "CLASSIFICATION") {
      pointAttributes.add(attributes::CLASSIFICATION);
    } else if (attribute == "NORMAL") {
      pointAttributes.add(attributes::NORMAL_OCT16);
    }
  }

  const auto attributesDescription = pointAttributes.toString();
  std::cout << "Writing the following point attributes: "
            << attributesDescription << std::endl;
}

void PotreeConverter::cleanUp() {
  const auto tempPath = workDir + "/temp";
  if (fs::exists(tempPath)) {
    fs::remove(tempPath);
  }
}

AABB PotreeConverter::calculateAABB() {
  AABB aabb;
  if (aabbValues.size() == 6) {
    Vector3<double> userMin(aabbValues[0], aabbValues[1], aabbValues[2]);
    Vector3<double> userMax(aabbValues[3], aabbValues[4], aabbValues[5]);
    aabb = AABB(userMin, userMax);
  } else {
    for (string source : sources) {
      PointReader *reader = createPointReader(source, pointAttributes);

      AABB sourceAABB = reader->getAABB();
      aabb.update(sourceAABB.min);
      aabb.update(sourceAABB.max);

      reader->close();
      delete reader;
    }
  }

  return aabb;
}

void PotreeConverter::generatePage(string name) {
  string pagedir = this->workDir;
  string templateSourcePath =
      this->executablePath + "/resources/page_template/viewer_template.html";
  string mapTemplateSourcePath =
      this->executablePath + "/resources/page_template/lasmap_template.html";
  string templateDir = this->executablePath + "/resources/page_template";

  if (!this->pageTemplatePath.empty()) {
    templateSourcePath = this->pageTemplatePath + "/viewer_template.html";
    mapTemplateSourcePath = this->pageTemplatePath + "/lasmap_template.html";
    templateDir = this->pageTemplatePath;
  }

  string templateTargetPath = pagedir + "/" + name + ".html";
  string mapTemplateTargetPath = pagedir + "/lasmap_" + name + ".html";

  Potree::copyDir(fs::path(templateDir), fs::path(pagedir));
  fs::remove(pagedir + "/viewer_template.html");
  fs::remove(pagedir + "/lasmap_template.html");

  if (!this->sourceListingOnly) {  // change viewer template
    ifstream in(templateSourcePath);
    ofstream out(templateTargetPath);

    string line;
    while (getline(in, line)) {
      if (line.find("<!-- INCLUDE POINTCLOUD -->") != string::npos) {
        out << "\t\tPotree.loadPointCloud(\"pointclouds/" << name
            << "/cloud.js\", \"" << name << "\", e => {" << endl;
        out << "\t\t\tlet pointcloud = e.pointcloud;\n";
        out << "\t\t\tlet material = pointcloud.material;\n";

        out << "\t\t\tviewer.scene.addPointCloud(pointcloud);" << endl;

        out << "\t\t\t"
            << "material.pointColorType = Potree.PointColorType." << material
            << "; // any Potree.PointColorType.XXXX \n";
        out << "\t\t\tmaterial.size = 1;\n";
        out << "\t\t\tmaterial.pointSizeType = "
               "Potree.PointSizeType.ADAPTIVE;\n";
        out << "\t\t\tmaterial.shape = Potree.PointShape.SQUARE;\n";

        out << "\t\t\tviewer.fitToScreen();" << endl;
        out << "\t\t});" << endl;
      } else if (line.find("<!-- INCLUDE SETTINGS HERE -->") != string::npos) {
        out << std::boolalpha;
        out << "\t\t"
            << "document.title = \"" << title << "\";\n";
        out << "\t\t"
            << "viewer.setEDLEnabled(" << edlEnabled << ");\n";
        if (showSkybox) {
          out << "\t\t"
              << "viewer.setBackground(\"skybox\"); // [\"skybox\", "
                 "\"gradient\", \"black\", \"white\"];\n";
        } else {
          out << "\t\t"
              << "viewer.setBackground(\"gradient\"); // [\"skybox\", "
                 "\"gradient\", \"black\", \"white\"];\n";
        }

        string descriptionEscaped = string(description);
        std::replace(descriptionEscaped.begin(), descriptionEscaped.end(), '`',
                     '\'');

        out << "\t\t"
            << "viewer.setDescription(`" << descriptionEscaped << "`);\n";
      } else {
        out << line << endl;
      }
    }

    in.close();
    out.close();
  }

  // change lasmap template
  // if(!this->projection.empty()){
  //	ifstream in( mapTemplateSourcePath );
  //	ofstream out( mapTemplateTargetPath );
  //
  //	string line;
  //	while(getline(in, line)){
  //		if(line.find("<!-- INCLUDE SOURCE -->") != string::npos){
  //			out << "\tvar source = \"" << "pointclouds/" << name <<
  //"/sources.json" << "\";"; 		}else{ 			out << line <<
  // endl;
  //		}
  //
  //	}
  //
  //	in.close();
  //	out.close();
  //}

  //{ // write settings
  //	stringstream ssSettings;
  //
  //	ssSettings << "var sceneProperties = {" << endl;
  //	ssSettings << "\tpath: \"" << "../resources/pointclouds/" << name <<
  //"/cloud.js\"," << endl;
  //	ssSettings << "\tcameraPosition: null, 		// other options:
  // cameraPosition: [10,10,10]," << endl; 	ssSettings << "\tcameraTarget:
  // null,
  //// other options: cameraTarget: [0,0,0]," << endl; 	ssSettings << "\tfov:
  /// 60, / field of view in degrees," << endl; 	ssSettings <<
  /// "\tsizeType:
  //\"Adaptive\",	// other options: \"Fixed\", \"Attenuated\"" << endl;
  //	ssSettings << "\tquality: null, 			// other
  // options:
  //\"Circles\", \"Interpolation\", \"Splats\"" << endl; 	ssSettings <<
  //"\tmaterial: \"RGB\", 		// other options: \"Height\",
  //\"Intensity\",
  //\"Classification\"" << endl; 	ssSettings << "\tpointLimit: 1,
  //// max number of points in millions" << endl; 	ssSettings <<
  ///"\tpointSize: 1, / " << endl; 	ssSettings << "\tnavigation: \"Orbit\",
  /// // other
  // options: \"Orbit\", \"Flight\"" << endl; 	ssSettings << "\tuseEDL: false,
  //" << endl; 	ssSettings << "};" << endl;
  //
  //
  //	ofstream fSettings;
  //	fSettings.open(pagedir + "/examples/" + name + ".js", ios::out);
  //	fSettings << ssSettings.str();
  //	fSettings.close();
  //}
}

static void writeSources(string path, vector<string> sourceFilenames,
                         vector<int> numPoints, vector<AABB> boundingBoxes,
                         string projection) {
  Document d(rapidjson::kObjectType);

  AABB bb;

  Value jProjection(projection.c_str(), (rapidjson::SizeType)projection.size());

  Value jSources(rapidjson::kObjectType);
  jSources.SetArray();
  for (int i = 0; i < sourceFilenames.size(); i++) {
    string &source = sourceFilenames[i];
    int points = numPoints[i];
    AABB boundingBox = boundingBoxes[i];

    bb.update(boundingBox);

    Value jSource(rapidjson::kObjectType);

    Value jName(source.c_str(), (rapidjson::SizeType)source.size());
    Value jPoints(points);
    Value jBounds(rapidjson::kObjectType);

    {
      Value bbMin(rapidjson::kObjectType);
      Value bbMax(rapidjson::kObjectType);

      bbMin.SetArray();
      bbMin.PushBack(boundingBox.min.x, d.GetAllocator());
      bbMin.PushBack(boundingBox.min.y, d.GetAllocator());
      bbMin.PushBack(boundingBox.min.z, d.GetAllocator());

      bbMax.SetArray();
      bbMax.PushBack(boundingBox.max.x, d.GetAllocator());
      bbMax.PushBack(boundingBox.max.y, d.GetAllocator());
      bbMax.PushBack(boundingBox.max.z, d.GetAllocator());

      jBounds.AddMember("min", bbMin, d.GetAllocator());
      jBounds.AddMember("max", bbMax, d.GetAllocator());
    }

    jSource.AddMember("name", jName, d.GetAllocator());
    jSource.AddMember("points", jPoints, d.GetAllocator());
    jSource.AddMember("bounds", jBounds, d.GetAllocator());

    jSources.PushBack(jSource, d.GetAllocator());
  }

  Value jBoundingBox(rapidjson::kObjectType);
  {
    Value bbMin(rapidjson::kObjectType);
    Value bbMax(rapidjson::kObjectType);

    bbMin.SetArray();
    bbMin.PushBack(bb.min.x, d.GetAllocator());
    bbMin.PushBack(bb.min.y, d.GetAllocator());
    bbMin.PushBack(bb.min.z, d.GetAllocator());

    bbMax.SetArray();
    bbMax.PushBack(bb.max.x, d.GetAllocator());
    bbMax.PushBack(bb.max.y, d.GetAllocator());
    bbMax.PushBack(bb.max.z, d.GetAllocator());

    jBoundingBox.AddMember("min", bbMin, d.GetAllocator());
    jBoundingBox.AddMember("max", bbMax, d.GetAllocator());
  }

  d.AddMember("bounds", jBoundingBox, d.GetAllocator());
  d.AddMember("projection", jProjection, d.GetAllocator());
  d.AddMember("sources", jSources, d.GetAllocator());

  StringBuffer buffer;
  // PrettyWriter<StringBuffer> writer(buffer);
  Writer<StringBuffer> writer(buffer);
  d.Accept(writer);

  if (!fs::exists(fs::path(path))) {
    fs::path pcdir(path);
    fs::create_directories(pcdir);
  }

  ofstream sourcesOut(path + "/sources.json", ios::out);
  sourcesOut << buffer.GetString();
  sourcesOut.close();
}

size_t PotreeConverter::get_total_points_count() const {
  size_t total_count = 0;
  for(auto& source : sources) {
    PointReader *reader = createPointReader(source, pointAttributes);
    total_count += reader->numPoints();
    delete reader;
  }
  return total_count;
}

void PotreeConverter::convert() {
  auto start = high_resolution_clock::now();

  prepare();

  size_t pointsProcessed = 0;
  size_t pointsSinceLastProcessing = 0;

  _ui_state.set_processed_points(0);
  _ui_state.set_total_points(get_total_points_count());

  // We don't transform the AABBs here, since this would break the process of
  // partitioning the points. Instead, we will transform only upon writing the
  // bounding boxes to the JSON files

  AABB aabb = calculateAABB();
  cout << "AABB: " << endl << aabb << endl;
  aabb.makeCubic();
  cout << "cubic AABB: " << endl << aabb << endl;

  auto transformation = get_transformation_helper(sourceProjection);

  if (diagonalFraction != 0) {
    spacing = (float)(aabb.size.length() / diagonalFraction);
    cout << "spacing calculated from diagonal: " << spacing << endl;
  }

  if (pageName.size() > 0) {
    generatePage(pageName);
    workDir = workDir + "/pointclouds/" + pageName;
  }

  PotreeWriter writer{this->workDir,   aabb,    spacing,
                      maxDepth,        scale,   outputFormat,
                      pointAttributes, quality, *transformation, max_memory_usage_MiB};

  vector<AABB> boundingBoxes;
  vector<int> numPoints;
  vector<string> sourceFilenames;

  ThroughputCounter point_throughput_counter;

  for (const auto &source : sources) {
    //cout << "READING:  " << source << endl;

    PointReader *reader = createPointReader(source, pointAttributes);
    // TODO We should issue a warning here if 'pointAttributes' do not match the
    // attributes available in the current file!

    boundingBoxes.push_back(reader->getAABB());
    numPoints.push_back(reader->numPoints());
    sourceFilenames.push_back(fs::path(source).filename().string());

    // NOTE Points from the source file(s) are read here
    while (true) {
      auto pointBatch = reader->readPointBatch();
      if (pointBatch.empty()) break;

      pointsProcessed += pointBatch.count();
      pointsSinceLastProcessing += pointBatch.count();
      point_throughput_counter.push_entry(pointBatch.count());
      
      _ui_state.set_current_mode("INDEXING");
      _ui_state.set_processed_points(pointsProcessed);
      _ui_state.set_progress(static_cast<float>(pointsProcessed) / _ui_state.get_total_points());
      _ui_state.set_points_per_second(static_cast<float>(point_throughput_counter.get_throughput_per_second()));
      _ui.redraw();

      writer.add(pointBatch);

      if (pointsSinceLastProcessing >= PROCESS_COUNT) {
        pointsSinceLastProcessing -= PROCESS_COUNT;

        writer.processStore();
        writer.waitUntilProcessed();

        auto end = high_resolution_clock::now();
        long long duration = duration_cast<milliseconds>(end - start).count();
        float seconds = duration / 1'000.0f;
      }
      if (writer.needs_flush()) {
        _ui_state.set_current_mode("FLUSHING");
        _ui.redraw();

        writer.flush();
      }
    }
    reader->close();
    delete reader;
  }

  _ui_state.set_current_mode("FINISHING");
  _ui.redraw();
  writer.flush();
  writer.close();


  float percent = (float)writer.numAccepted / (float)pointsProcessed;
  percent = percent * 100;

  auto end = high_resolution_clock::now();
  long long duration = duration_cast<milliseconds>(end - start).count();

  std::stringstream ss;
  ss << "Conversion finished! " << pointsProcessed << " points processed, " << writer.numAccepted << " points (" << std::fixed <<
   std::setprecision(2) << percent << " %) written to output. Took " << (duration / 1000.0f) << "s.";

  _ui_state.push_message(ss.str());
  _ui_state.set_current_mode("DONE");
  _ui.redraw();
}

}  // namespace Potree
