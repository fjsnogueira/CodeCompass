#include <algorithm>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <iterator>

#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>

#include <model/buildaction.h>
#include <model/buildaction-odb.hxx>
#include <model/buildsourcetarget.h>
#include <model/buildsourcetarget-odb.hxx>
#include <model/file.h>
#include <model/file-odb.hxx>

#include <util/hash.h>
#include <util/logutil.h>
#include <util/odbtransaction.h>
#include <util/threadpool.h>

#include <cppparser/cppparser.h>

#include "clangastvisitor.h"
#include "relationcollector.h"
#include "manglednamecache.h"
#include "ppincludecallback.h"
#include "ppmacrocallback.h"
#include "doccommentcollector.h"

namespace cc
{
namespace parser
{

class VisitorActionFactory : public clang::tooling::FrontendActionFactory
{
public:
  static void cleanUp()
  {
    MyFrontendAction::_mangledNameCache.clear();
  }

  static void init(ParserContext& ctx_)
  {
    (util::OdbTransaction(ctx_.db))([&] {
      for (const model::CppAstNode& node : ctx_.db->query<model::CppAstNode>())
        MyFrontendAction::_mangledNameCache.insert(node);
    });
  }

  VisitorActionFactory(ParserContext& ctx_) : _ctx(ctx_)
  {
  }

  clang::FrontendAction* create() override
  {
    return new MyFrontendAction(_ctx);
  }

private:
  class MyConsumer : public clang::ASTConsumer
  {
  public:
    MyConsumer(
      ParserContext& ctx_,
      clang::ASTContext& context_,
      MangledNameCache& mangledNameCache_)
        : _mangledNameCache(mangledNameCache_), _ctx(ctx_), _context(context_)
    {
    }

    virtual void HandleTranslationUnit(clang::ASTContext& context_) override
    {
      {
        ClangASTVisitor clangAstVisitor(
          _ctx, _context, _mangledNameCache, _clangToAstNodeId);
        clangAstVisitor.TraverseDecl(context_.getTranslationUnitDecl());
      }

      {
        RelationCollector relationCollector(
          _ctx, _context);
        relationCollector.TraverseDecl(context_.getTranslationUnitDecl());
      }

      if (!_ctx.options.count("skip-doccomment"))
      {
        DocCommentCollector docCommentCollector(
          _ctx, _context, _mangledNameCache, _clangToAstNodeId);
        docCommentCollector.TraverseDecl(context_.getTranslationUnitDecl());
      }
      else
        LOG(info) << "C++ documentation parser has been skipped.";
    }

  private:
    MangledNameCache& _mangledNameCache;
    std::unordered_map<const void*, model::CppAstNodeId> _clangToAstNodeId;

    ParserContext& _ctx;
    clang::ASTContext& _context;
  };

  class MyFrontendAction : public clang::ASTFrontendAction
  {
    friend class VisitorActionFactory;

  public:
    MyFrontendAction(ParserContext& ctx_) : _ctx(ctx_)
    {
    }

    virtual bool BeginSourceFileAction(
      clang::CompilerInstance& compiler_) override
    {
      compiler_.createASTContext();
      auto& pp = compiler_.getPreprocessor();

      pp.addPPCallbacks(std::make_unique<PPIncludeCallback>(
        _ctx, compiler_.getASTContext(), _mangledNameCache, pp));
      pp.addPPCallbacks(std::make_unique<PPMacroCallback>(
        _ctx, compiler_.getASTContext(), _mangledNameCache, pp));

      return true;
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
      clang::CompilerInstance& compiler_, llvm::StringRef) override
    {
      return std::unique_ptr<clang::ASTConsumer>(
        new MyConsumer(_ctx, compiler_.getASTContext(), _mangledNameCache));
    }

  private:
    static MangledNameCache _mangledNameCache;

    ParserContext& _ctx;
  };

  ParserContext& _ctx;
};

MangledNameCache VisitorActionFactory::MyFrontendAction::_mangledNameCache;

bool CppParser::isSourceFile(const std::string& file_) const
{
  const std::vector<std::string> cppExts{
    ".c", ".cc", ".cpp", ".cxx", ".o", ".so", ".a"};

  std::string ext = boost::filesystem::extension(file_);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  return std::find(cppExts.begin(), cppExts.end(), ext) != cppExts.end();
}

bool CppParser::isNonSourceFlag(const std::string& arg_) const
{
  return arg_.find("-Wl,") == 0;
}

std::map<std::string, std::string> CppParser::extractInputOutputs(
  const clang::tooling::CompileCommand& command_) const
{
  std::map<std::string, std::string> inToOut;

  enum State
  {
    None,
    OParam
  };

  bool hasCParam = false;
  std::unordered_set<std::string> sources;
  std::string output;

  State state = None;
  for (const std::string& arg : command_.CommandLine)
  {
    if (state == OParam)
    {
      boost::filesystem::path absolutePath =
        boost::filesystem::absolute(arg, command_.Directory);

      output = absolutePath.native();
      state = None;
    }
    else if (isSourceFile(arg) && !isNonSourceFlag(arg))
    {
      boost::filesystem::path absolutePath =
        boost::filesystem::absolute(arg, command_.Directory);
      sources.insert(absolutePath.native());
    }
    else if (arg == "-c")
      hasCParam = true;
    else if (arg == "-o")
      state = OParam;
  }

  if (output.empty() && hasCParam)
  {
    for (const std::string& src : sources)
    {
      std::string extension = boost::filesystem::extension(src);
      inToOut[src] = src.substr(0, src.size() - extension.size() - 1) + ".o";
    }
  }
  else
  {
    if (output.empty())
      output = command_.Directory + "/a.out";

    for (const std::string& src : sources)
      inToOut[src] = output;
  }

  return inToOut;
}

model::BuildActionPtr CppParser::addBuildAction(
  const clang::tooling::CompileCommand& command_)
{
  util::OdbTransaction transaction(_ctx.db);

  model::BuildActionPtr buildAction(new model::BuildAction);

  std::string extension = boost::filesystem::extension(command_.Filename);

  buildAction->command = boost::algorithm::join(command_.CommandLine, " ");
  buildAction->type
    = extension == ".o" || extension == ".so" || extension == ".a"
    ? model::BuildAction::Link
    : model::BuildAction::Compile;

  transaction([&, this]{ _ctx.db->persist(buildAction); });

  return buildAction;
}

void CppParser::addCompileCommand(
  const clang::tooling::CompileCommand& command_,
  model::BuildActionPtr buildAction_,
  bool error_)
{
  util::OdbTransaction transaction(_ctx.db);

  std::vector<model::BuildSource> sources;
  std::vector<model::BuildTarget> targets;

  for (const auto& srcTarget : extractInputOutputs(command_))
  {
    model::BuildSource buildSource;
    buildSource.file = _ctx.srcMgr.getFile(srcTarget.first);
    buildSource.file->parseStatus = error_
      ? model::File::PSPartiallyParsed
      : model::File::PSFullyParsed;
    _ctx.srcMgr.updateFile(*buildSource.file);
    buildSource.action = buildAction_;
    sources.push_back(std::move(buildSource));

    model::BuildTarget buildTarget;
    buildTarget.file = _ctx.srcMgr.getFile(srcTarget.second);
    buildTarget.action = buildAction_;
    if (buildTarget.file->type != model::File::BINARY_TYPE)
    {
      buildTarget.file->type = model::File::BINARY_TYPE;
      _ctx.srcMgr.updateFile(*buildTarget.file);
    }

    targets.push_back(std::move(buildTarget));
  }

  _ctx.srcMgr.persistFiles();

  transaction([&, this] {
    for (model::BuildSource buildSource : sources)
      _ctx.db->persist(buildSource);
    for (model::BuildTarget buildTarget : targets)
      _ctx.db->persist(buildTarget);
  });
}

int CppParser::worker(const clang::tooling::CompileCommand& command_)
{
  //--- Assemble compiler command line ---//

  std::vector<const char*> commandLine;
  commandLine.reserve(command_.CommandLine.size());
  commandLine.push_back("--");
  std::transform(
    command_.CommandLine.begin() + 1, // Skip compiler name
    command_.CommandLine.end(),
    std::back_inserter(commandLine),
    [](const std::string& s){ return s.c_str(); });

  int argc = commandLine.size();

  std::string compilationDbLoadError;
  std::unique_ptr<clang::tooling::FixedCompilationDatabase> compilationDb(
    clang::tooling::FixedCompilationDatabase::loadFromCommandLine(
      argc,
      commandLine.data(),
      compilationDbLoadError));

  if (!compilationDb)
  {
    LOG(error) << "Failed to create compilation database from command-line. " << compilationDbLoadError;
    return 1;
  }

  //--- Save build action ---//

  model::BuildActionPtr buildAction = addBuildAction(command_);

  //--- Start the tool ---//

  VisitorActionFactory factory(_ctx);
  clang::tooling::ClangTool tool(*compilationDb, command_.Filename);

  int error = tool.run(&factory);

  //--- Save build command ---//

  addCompileCommand(command_, buildAction, error);

  return error;
}

CppParser::CppParser(ParserContext& ctx_) : AbstractParser(ctx_)
{
}

std::vector<std::string> CppParser::getDependentParsers() const
{
  return std::vector<std::string>{};
}

bool CppParser::parse()
{
  if(_ctx.options.count("incremental"))
  {
    LOG(info) << "Incremental Parsing Enabled";
    incrementalParse();
  }

  initBuildActions();
  VisitorActionFactory::init(_ctx);

  bool success = true;

  for (const std::string& input
    : _ctx.options["input"].as<std::vector<std::string>>())
    if (boost::filesystem::is_regular_file(input))
      success
        = success && parseByJson(input, _ctx.options["jobs"].as<int>());

  VisitorActionFactory::cleanUp();
  _parsedCommandHashes.clear();

  return success;
}

void CppParser::incrementalParse()
{
  std::unordered_map<std::string, std::string> fileHashes;

  (util::OdbTransaction(_ctx.db))([&] {
    auto files = _ctx.db->query<model::File>(
      odb::query<model::File>::type != model::File::DIRECTORY_TYPE &&
      odb::query<model::File>::type != model::File::BINARY_TYPE);

    for(const model::File& file : files)
    {
      if (boost::filesystem::exists(file.path))
      {
        if (!_fileStatus.count(file.path))
        {
          auto content = file.content.load();
          fileHashes[file.path] = content != nullptr ? content->hash : "";
          if (content == nullptr)
            continue;

          std::ifstream fileStream(file.path);
          std::string fileContent(
            (std::istreambuf_iterator<char>(fileStream)),
            (std::istreambuf_iterator<char>()));
          fileStream.close();

          if (content->hash != util::sha1Hash(fileContent))
          {
            markAsModified(file);
          }
        }
      }
      else
      {
        _fileStatus.insert(std::make_pair(file.path, IncrementalStatus::DELETED));
        LOG(debug) << "File deleted: " << file.path;
      }
    }

    // TODO: detect added files (not necessary currently)

    for(auto& item : _fileStatus)
    {
      switch (item.second)
      {
        case IncrementalStatus::MODIFIED:
        case IncrementalStatus::DELETED:
        {
          LOG(info) << "Database cleanup: " << item.first;

          // Query file
          auto delFile = _ctx.db->query<model::File>(
            odb::query<model::File>::path == item.first).one();

          // Query CppAstNode
          auto defCppAstNodes = _ctx.db->query<model::CppAstNode>(
            odb::query<model::CppAstNode>::location.file == delFile->id &&
            odb::query<model::CppAstNode>::astType == model::CppAstNode::AstType::Definition);
          for (const model::CppAstNode &astNode : defCppAstNodes)
          {
            // Delete CppEntity
            auto delCppEntities = _ctx.db->query<model::CppEntity>(
              odb::query<model::CppEntity>::mangledNameHash == astNode.mangledNameHash);
            for (const model::CppEntity &entity : delCppEntities)
            {
              _ctx.db->erase<model::CppEntity>(entity.id);
            }

            // Delete CppInheritance
            auto delCppInheritance = _ctx.db->query<model::CppInheritance>(
              odb::query<model::CppInheritance>::derived == astNode.mangledNameHash);
            for (const model::CppInheritance &inheritance : delCppInheritance)
            {
              _ctx.db->erase<model::CppInheritance>(inheritance.id);
            }

            // Delete CppFriendship
            auto delCppFriendship = _ctx.db->query<model::CppFriendship>(
              odb::query<model::CppFriendship>::target == astNode.mangledNameHash);
            for (const model::CppFriendship &friendship : delCppFriendship)
            {
              _ctx.db->erase<model::CppFriendship>(friendship.id);
            }

            // Delete CppNode (connected to CppAstNode) with all its connected CppNodes
            auto delNodes = _ctx.db->query<model::CppNode>(
              odb::query<model::CppNode>::domainId == std::to_string(astNode.id) &&
              odb::query<model::CppNode>::domain == model::CppNode::CPPASTNODE);
            for (model::CppNode &node : delNodes)
            {
              for (model::CppNodeId nodeId : collectNodeSet(node.id))
              {
                _ctx.db->erase<model::CppNode>(nodeId);
              }
            }
          }

          // Delete BuildAction
          auto delSources = _ctx.db->query<model::BuildSource>(
            odb::query<model::BuildSource>::file == delFile->id);
          for (const model::BuildSource &source : delSources)
          {
            _ctx.db->erase<model::BuildAction>(source.action->id);
          }

          // Delete CppNode (connected to File) with all its connected CppNodes
          auto delNodes = _ctx.db->query<model::CppNode>(
            odb::query<model::CppNode>::domainId == std::to_string(delFile->id) &&
            odb::query<model::CppNode>::domain == model::CppNode::FILE);
          for (model::CppNode &node : delNodes)
          {
            for (model::CppNodeId nodeId : collectNodeSet(node.id))
            {
              _ctx.db->erase<model::CppNode>(nodeId);
            }
          }

          // Delete File and FileContent (only when no other File references it)
          _ctx.srcMgr.removeFile(*delFile);

          break;
        }

        case IncrementalStatus::ADDED:
          // Empty deliberately
          break;
      }
    }
  }); // end of transaction
}

void CppParser::initBuildActions()
{
  (util::OdbTransaction(_ctx.db))([&, this] {
    for (const model::BuildAction& ba : _ctx.db->query<model::BuildAction>())
      _parsedCommandHashes.insert(util::fnvHash(ba.command));
  });
}

void CppParser::markAsModified(const model::File& file_)
{
  if(_fileStatus.count(file_.path) == 0)
  {
    _fileStatus.insert(std::make_pair(file_.path, IncrementalStatus::MODIFIED));
    LOG(debug) << "File modified: " << file_.path;

    auto inclusions = _ctx.db->query<model::CppHeaderInclusion>(
      odb::query<model::CppHeaderInclusion>::included == file_.id);

    for (auto inc : inclusions)
    {
      markAsModified(*inc.includer.load());
    }
  }
}

std::set<model::CppNodeId> CppParser::collectNodeSet(model::CppNodeId node_) const
{
  std::set<model::CppNodeId> nodes;
  std::queue<model::CppNodeId> processQueue;

  nodes.insert(node_);
  processQueue.push(node_);

  while(!processQueue.empty())
  {
    auto nodeId = processQueue.front();
    processQueue.pop();

    // Fetch nodes on edges where current node has a 'from' role
    auto fromEdges = _ctx.db->query<model::CppEdge>(
      odb::query<model::CppEdge>::from == nodeId);
    for (const model::CppEdge &edge : fromEdges)
    {
      if (!nodes.count(edge.to->id))
      {
        nodes.insert(edge.to->id);
        processQueue.push(edge.to->id);
      }
    }

    // Fetch nodes on edges where current node has a 'to' role
    auto toEdges = _ctx.db->query<model::CppEdge>(
      odb::query<model::CppEdge>::to == nodeId);
    for (const model::CppEdge &edge : toEdges)
    {
      if (!nodes.count(edge.from->id))
      {
        nodes.insert(edge.from->id);
        processQueue.push(edge.from->id);
      }
    }
  }

  return nodes;
}

bool CppParser::parseByJson(
  const std::string& jsonFile_,
  std::size_t threadNum_)
{
  std::string errorMsg;

  std::unique_ptr<clang::tooling::JSONCompilationDatabase> compDb
    = clang::tooling::JSONCompilationDatabase::loadFromFile(
        jsonFile_, errorMsg,
        clang::tooling::JSONCommandLineSyntax::Gnu);

  if (!errorMsg.empty())
  {
    LOG(error) << errorMsg;
    return false;
  }

  //--- Read the compilation commands compile database ---//

  std::vector<clang::tooling::CompileCommand> compileCommands =
    compDb->getAllCompileCommands();
  std::size_t numCompileCommands = compileCommands.size();

  //--- Create a thread pool for the current commands ---//
  std::unique_ptr<
    util::JobQueueThreadPool<ParseJob>> pool =
    util::make_thread_pool<ParseJob>(
      threadNum_, [this, &numCompileCommands](ParseJob& job_)
      {
        const clang::tooling::CompileCommand& command = job_.command;

        LOG(info)
          << '(' << job_.index << '/' << numCompileCommands << ')'
          << " Parsing " << command.Filename;

        int error = this->worker(command);

        if (error)
          LOG(warning)
            << '(' << job_.index << '/' << numCompileCommands << ')'
            << " Parsing " << command.Filename << " has been failed.";
      });

  //--- Push all commands into the thread pool's queue ---//
  std::size_t index = 0;

  for (const auto& command : compileCommands)
  {
    ParseJob job(command, ++index);

    auto hash = util::fnvHash(
      boost::algorithm::join(command.CommandLine, " "));

    if (_parsedCommandHashes.find(hash) != _parsedCommandHashes.end())
    {
      LOG(info)
        << '(' << index << '/' << numCompileCommands << ')'
        << " Already parsed " << command.Filename;

      continue;
    }

    //--- Add compile command hash ---//

    _parsedCommandHashes.insert(hash);

    //--- Push the job ---//

    pool->enqueue(job);
  }

  // Block execution until every job is finished.
  pool->wait();

  return true;
}

CppParser::~CppParser()
{
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C"
{
  boost::program_options::options_description getOptions()
  {
    boost::program_options::options_description description("C++ Plugin");
    description.add_options()
      ("skip-doccomment",
       "If this flag is given the parser will skip parsing the documentation "
       "comments.")
      ("incremental",
       "Enable incremental parsing.");
    return description;
  }

  std::shared_ptr<CppParser> make(ParserContext& ctx_)
  {
    return std::make_shared<CppParser>(ctx_);
  }
}
#pragma clang diagnostic pop

} // parser
} // cc
