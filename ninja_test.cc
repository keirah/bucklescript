#include "ninja.h"

#include <gtest/gtest.h>

#include "build.h"
#include "parsers.h"
#include "test.h"

static void AssertParse(State* state, const char* input) {
  ManifestParser parser(state, NULL);
  string err;
  ASSERT_TRUE(parser.Parse(input, &err)) << err;
  ASSERT_EQ("", err);
}

StateTestWithBuiltinRules::StateTestWithBuiltinRules() {
  AssertParse(&state_,
"rule cat\n"
"  command = cat $in > $out\n");
}

Node* StateTestWithBuiltinRules::GetNode(const string& path) {
  return state_.stat_cache()->GetFile(path)->node_;
}

TEST(State, Basic) {
  State state;
  Rule* rule = new Rule("cat");
  string err;
  EXPECT_TRUE(rule->ParseCommand("cat $in > $out", &err));
  ASSERT_EQ("", err);
  state.AddRule(rule);
  Edge* edge = state.AddEdge(rule);
  state.AddInOut(edge, Edge::IN, "in1");
  state.AddInOut(edge, Edge::IN, "in2");
  state.AddInOut(edge, Edge::OUT, "out");

  EXPECT_EQ("cat in1 in2 > out", edge->EvaluateCommand());

  EXPECT_FALSE(state.GetNode("in1")->dirty());
  EXPECT_FALSE(state.GetNode("in2")->dirty());
  EXPECT_FALSE(state.GetNode("out")->dirty());

  state.stat_cache()->GetFile("in1")->Touch(1);
  EXPECT_TRUE(state.GetNode("in1")->dirty());
  EXPECT_FALSE(state.GetNode("in2")->dirty());
  EXPECT_TRUE(state.GetNode("out")->dirty());
}

struct TestEnv : public Env {
  virtual string LookupVariable(const string& var) {
    return vars[var];
  }
  map<string, string> vars;
};
TEST(EvalString, PlainText) {
  EvalString str;
  string err;
  EXPECT_TRUE(str.Parse("plain text", &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("plain text", str.Evaluate(NULL));
}
TEST(EvalString, OneVariable) {
  EvalString str;
  string err;
  EXPECT_TRUE(str.Parse("hi $var", &err));
  EXPECT_EQ("", err);
  EXPECT_EQ("hi $var", str.unparsed());
  TestEnv env;
  EXPECT_EQ("hi ", str.Evaluate(&env));
  env.vars["var"] = "there";
  EXPECT_EQ("hi there", str.Evaluate(&env));
}
TEST(EvalString, Error) {
  EvalString str;
  string err;
  EXPECT_FALSE(str.Parse("bad $", &err));
  EXPECT_EQ("expected variable after $", err);
}
TEST(EvalString, Curlies) {
  EvalString str;
  string err;
  EXPECT_TRUE(str.Parse("foo ${var}baz", &err));
  EXPECT_EQ("", err);
  TestEnv env;
  EXPECT_EQ("foo baz", str.Evaluate(&env));
  env.vars["var"] = "barbar";
  EXPECT_EQ("foo barbarbaz", str.Evaluate(&env));
}

// Though Plan doesn't use State, it's useful to have one around
// to create Nodes and Edges.
struct PlanTest : public StateTestWithBuiltinRules {
  Plan plan_;
};

TEST_F(PlanTest, Basic) {
  AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n");
  GetNode("in")->MarkDependentsDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("in",  edge->inputs_[0]->file_->path_);
  ASSERT_EQ("mid", edge->outputs_[0]->file_->path_);

  ASSERT_FALSE(plan_.FindWork());

  GetNode("mid")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);
  ASSERT_EQ("mid", edge->inputs_[0]->file_->path_);
  ASSERT_EQ("out", edge->outputs_[0]->file_->path_);

  GetNode("out")->dirty_ = false;
  plan_.EdgeFinished(edge);

  ASSERT_FALSE(plan_.more_to_do());
  edge = plan_.FindWork();
  ASSERT_EQ(NULL, edge);
}

// Test that two outputs from one rule can be handled as inputs to the next.
TEST_F(PlanTest, DoubleOutputDirect) {
  AssertParse(&state_,
"build out: cat mid1 mid2\n"
"build mid1 mid2: cat in\n");
  GetNode("in")->MarkDependentsDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  GetNode("mid1")->dirty_ = false;
  GetNode("mid2")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid1 mid2
  GetNode("in")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

// Test that two outputs from one rule can eventually be routed to another.
TEST_F(PlanTest, DoubleOutputIndirect) {
  AssertParse(&state_,
"build out: cat b1 b2\n"
"build b1: cat a1\n"
"build b2: cat a2\n"
"build a1 a2: cat in\n");
  GetNode("in")->MarkDependentsDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  GetNode("a1")->dirty_ = false;
  GetNode("a2")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a1
  GetNode("b1")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a2
  GetNode("b2")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat b1 b2
  GetNode("out")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

// Test that two edges from one output can both execute.
TEST_F(PlanTest, DoubleDependent) {
  AssertParse(&state_,
"build out: cat a1 a2\n"
"build a1: cat mid\n"
"build a2: cat mid\n"
"build mid: cat in\n");
  GetNode("in")->MarkDependentsDirty();
  string err;
  EXPECT_TRUE(plan_.AddTarget(GetNode("out"), &err));
  ASSERT_EQ("", err);
  ASSERT_TRUE(plan_.more_to_do());

  Edge* edge;
  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat in
  GetNode("mid")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid
  GetNode("a1")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat mid
  GetNode("a2")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_TRUE(edge);  // cat a1 a2
  GetNode("out")->dirty_ = false;
  plan_.EdgeFinished(edge);

  edge = plan_.FindWork();
  ASSERT_FALSE(edge);  // done
}

struct BuildTest : public StateTestWithBuiltinRules,
                   public Shell,
                   public DiskInterface {
  BuildTest() : builder_(&state_), now_(1) {
    builder_.disk_interface_ = this;
    AssertParse(&state_,
"build cat1: cat in1\n"
"build cat2: cat in1 in2\n"
"build cat12: cat cat1 cat2\n");
  }

  // Mark a path dirty.
  void Dirty(const string& path);
  // Mark dependents of a path dirty.
  void Touch(const string& path);

  // shell override
  virtual bool RunCommand(Edge* edge);

  // DiskInterface
  virtual int Stat(const string& path) {
    return now_;
  }
  virtual bool MakeDir(const string& path) {
    directories_made_.push_back(path);
    return true;  // success
  }
  virtual string ReadFile(const string& path, string* err) {
    files_read_.push_back(path);
    map<string, string>::iterator i = file_contents_.find(path);
    if (i != file_contents_.end())
      return i->second;
    return "";
  }

  Builder builder_;
  int now_;
  map<string, string> file_contents_;
  vector<string> commands_ran_;
  vector<string> directories_made_;
  vector<string> files_read_;
};

void BuildTest::Dirty(const string& path) {
  Node* node = GetNode(path);
  node->MarkDirty();

  // If it's an input file, mark that we've already stat()ed it and
  // it's missing.
  if (!node->in_edge_)
    node->file_->mtime_ = 0;
}

void BuildTest::Touch(const string& path) {
  Node* node = GetNode(path);
  assert(node);
  node->MarkDependentsDirty();
}

bool BuildTest::RunCommand(Edge* edge) {
  commands_ran_.push_back(edge->EvaluateCommand());
  if (edge->rule_->name_ == "cat" || edge->rule_->name_ == "cc") {
    for (vector<Node*>::iterator out = edge->outputs_.begin();
         out != edge->outputs_.end(); ++out) {
      (*out)->file_->Touch(now_);
    }
    return true;
  } else {
    printf("unkown command\n");
  }

  return false;
}

TEST_F(BuildTest, NoWork) {
  string err;
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("no work to do", err);
  EXPECT_EQ(0, commands_ran_.size());
}

TEST_F(BuildTest, OneStep) {
  // Given a dirty target with one ready input,
  // we should rebuild the target.
  Dirty("cat1");
  string err;
  ASSERT_TRUE(builder_.AddTarget("cat1", &err));
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("", err);

  ASSERT_EQ(1, commands_ran_.size());
  EXPECT_EQ("cat in1 > cat1", commands_ran_[0]);
}

TEST_F(BuildTest, OneStep2) {
  // Given a target with one dirty input,
  // we should rebuild the target.
  Dirty("cat1");
  string err;
  EXPECT_TRUE(builder_.AddTarget("cat1", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("", err);

  ASSERT_EQ(1, commands_ran_.size());
  EXPECT_EQ("cat in1 > cat1", commands_ran_[0]);
}

TEST_F(BuildTest, TwoStep) {
  // Modifying in1 requires rebuilding both intermediate files
  // and the final file.
  Touch("in1");
  string err;
  EXPECT_TRUE(builder_.AddTarget("cat12", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(3, commands_ran_.size());
  EXPECT_EQ("cat in1 > cat1", commands_ran_[0]);
  EXPECT_EQ("cat in1 in2 > cat2", commands_ran_[1]);
  EXPECT_EQ("cat cat1 cat2 > cat12", commands_ran_[2]);

  // Modifying in2 requires rebuilding one intermediate file
  // and the final file.
  Touch("in2");
  ASSERT_TRUE(builder_.AddTarget("cat12", &err));
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(5, commands_ran_.size());
  EXPECT_EQ("cat in1 in2 > cat2", commands_ran_[3]);
  EXPECT_EQ("cat cat1 cat2 > cat12", commands_ran_[4]);
}

TEST_F(BuildTest, Chain) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build c2: cat c1\n"
"build c3: cat c2\n"
"build c4: cat c3\n"
"build c5: cat c4\n"));

  Touch("c1");  // Will recursively dirty all the way to c5.
  string err;
  EXPECT_TRUE(builder_.AddTarget("c5", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(4, commands_ran_.size());

  err.clear();
  commands_ran_.clear();
  EXPECT_FALSE(builder_.AddTarget("c5", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ(0, commands_ran_.size());

  Touch("c3");  // Will recursively dirty through to c5.
  err.clear();
  commands_ran_.clear();
  EXPECT_TRUE(builder_.AddTarget("c5", &err));
  ASSERT_EQ("", err);
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ(2, commands_ran_.size());  // 3->4, 4->5
}

TEST_F(BuildTest, MissingInput) {
  // Input is referenced by build file, but no rule for it.
  string err;
  Dirty("in1");
  EXPECT_FALSE(builder_.AddTarget("cat1", &err));
  EXPECT_EQ("'in1' missing and no known rule to make it", err);
}

TEST_F(BuildTest, MissingTarget) {
  // Target is not referenced by build file.
  string err;
  EXPECT_FALSE(builder_.AddTarget("meow", &err));
  EXPECT_EQ("unknown target: 'meow'", err);
}

TEST_F(BuildTest, MakeDirs) {
  string err;
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build subdir/dir2/file: cat in1\n"));

  Touch("in1");
  EXPECT_TRUE(builder_.AddTarget("subdir/dir2/file", &err));
  EXPECT_EQ("", err);
  now_ = 0;  // Make all stat()s return file not found.
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(2, directories_made_.size());
  EXPECT_EQ("subdir", directories_made_[0]);
  EXPECT_EQ("subdir/dir2", directories_made_[1]);
}

TEST_F(BuildTest, DepFileMissing) {
  string err;
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cc\n  command = cc $in\n  depfile = $out.d\n"
"build foo.o: cc foo.c\n"));
  Touch("foo.c");
  EXPECT_TRUE(builder_.AddTarget("foo.o", &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(1, files_read_.size());
  EXPECT_EQ("foo.o.d", files_read_[0]);
}

TEST_F(BuildTest, DepFileOK) {
  string err;
  int orig_edges = state_.edges_.size();
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cc\n  command = cc $in\n  depfile = $out.d\n"
"build foo.o: cc foo.c\n"));
  Touch("foo.c");
  file_contents_["foo.o.d"] = "foo.o: blah.h bar.h\n";
  EXPECT_TRUE(builder_.AddTarget("foo.o", &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(1, files_read_.size());
  EXPECT_EQ("foo.o.d", files_read_[0]);

  // Expect our edge to now have three inputs: foo.c and two headers.
  ASSERT_EQ(orig_edges + 1, state_.edges_.size());
  Edge* edge = state_.edges_.back();
  ASSERT_EQ(3, edge->inputs_.size());

  // Expect the command line we generate to only use the original input.
  ASSERT_EQ("cc foo.c", edge->EvaluateCommand());
}

TEST_F(BuildTest, DepFileParseError) {
  string err;
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cc\n  command = cc $in\n  depfile = $out.d\n"
"build foo.o: cc foo.c\n"));
  Touch("foo.c");
  file_contents_["foo.o.d"] = "foo.o blah.h bar.h\n";
  EXPECT_FALSE(builder_.AddTarget("foo.o", &err));
  EXPECT_EQ("line 1, col 7: expected ':', got 'blah.h'", err);
}

TEST_F(BuildTest, OrderOnlyDeps) {
  string err;
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"rule cc\n  command = cc $in\n  depfile = $out.d\n"
"build foo.o: cc foo.c | otherfile\n"));
  Touch("foo.c");
  file_contents_["foo.o.d"] = "foo.o: blah.h bar.h\n";
  EXPECT_TRUE(builder_.AddTarget("foo.o", &err));

  Edge* edge = state_.edges_.back();
  // One explicit, two implicit, one order only.
  ASSERT_EQ(4, edge->inputs_.size());
  EXPECT_EQ(2, edge->implicit_deps_);
  EXPECT_EQ(1, edge->order_only_deps_);
  // Verify the inputs are in the order we expect
  // (explicit then implicit then orderonly).
  EXPECT_EQ("foo.c", edge->inputs_[0]->file_->path_);
  EXPECT_EQ("blah.h", edge->inputs_[1]->file_->path_);
  EXPECT_EQ("bar.h", edge->inputs_[2]->file_->path_);
  EXPECT_EQ("otherfile", edge->inputs_[3]->file_->path_);

  // Expect the command line we generate to only use the original input.
  ASSERT_EQ("cc foo.c", edge->EvaluateCommand());

  // explicit dep dirty, expect a rebuild.
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(1, commands_ran_.size());

  // implicit dep dirty, expect a rebuild.
  commands_ran_.clear();
  Touch("blah.h");
  EXPECT_TRUE(builder_.AddTarget("foo.o", &err));
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(1, commands_ran_.size());

  // order only dep dirty, no rebuild.
  commands_ran_.clear();
  Touch("otherfile");
  // We should fail to even add the depenency on foo.o, because
  // there's nothing to do.
  EXPECT_FALSE(builder_.AddTarget("foo.o", &err));
}

TEST_F(BuildTest, Phony) {
  string err;
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat bar.cc\n"
"build all: phony out\n"));
  Touch("bar.cc");

  EXPECT_TRUE(builder_.AddTarget("all", &err));

  // Only one command to run, because phony runs no command.
  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(1, commands_ran_.size());

  EXPECT_TRUE(builder_.Build(this, &err));
  ASSERT_NE("", err);
}

struct StatTest : public StateTestWithBuiltinRules,
                  public DiskInterface {
  // DiskInterface implementation.
  virtual int Stat(const string& path);
  virtual bool MakeDir(const string& path) {
    assert(false);
    return false;
  }
  virtual string ReadFile(const string& path, string* err) {
    assert(false);
    return "";
  }

  map<string, time_t> mtimes_;
  vector<string> stats_;
};

int StatTest::Stat(const string& path) {
  stats_.push_back(path);
  map<string, time_t>::iterator i = mtimes_.find(path);
  if (i == mtimes_.end())
    return 0;  // File not found.
  return i->second;
}

TEST_F(StatTest, Simple) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat in\n"));

  Node* out = GetNode("out");
  out->file_->Stat(this);
  ASSERT_EQ(1, stats_.size());
  Edge* edge = out->in_edge_;
  edge->RecomputeDirty(NULL, this, NULL);
  ASSERT_EQ(2, stats_.size());
  ASSERT_EQ("out", stats_[0]);
  ASSERT_EQ("in",  stats_[1]);
}

TEST_F(StatTest, TwoStep) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n"));

  Node* out = GetNode("out");
  out->file_->Stat(this);
  ASSERT_EQ(1, stats_.size());
  Edge* edge = out->in_edge_;
  edge->RecomputeDirty(NULL, this, NULL);
  ASSERT_EQ(3, stats_.size());
  ASSERT_EQ("out", stats_[0]);
  ASSERT_TRUE(GetNode("out")->dirty_);
  ASSERT_EQ("mid",  stats_[1]);
  ASSERT_TRUE(GetNode("mid")->dirty_);
  ASSERT_EQ("in",  stats_[2]);
}

TEST_F(StatTest, Tree) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid1 mid2\n"
"build mid1: cat in11 in12\n"
"build mid2: cat in21 in22\n"));

  Node* out = GetNode("out");
  out->file_->Stat(this);
  ASSERT_EQ(1, stats_.size());
  Edge* edge = out->in_edge_;
  edge->RecomputeDirty(NULL, this, NULL);
  ASSERT_EQ(1 + 6, stats_.size());
  ASSERT_EQ("mid1", stats_[1]);
  ASSERT_TRUE(GetNode("mid1")->dirty_);
  ASSERT_EQ("in11", stats_[2]);
}

TEST_F(StatTest, Middle) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n"));

  mtimes_["in"] = 1;
  mtimes_["mid"] = 0;  // missing
  mtimes_["out"] = 1;

  Node* out = GetNode("out");
  out->file_->Stat(this);
  ASSERT_EQ(1, stats_.size());
  Edge* edge = out->in_edge_;
  edge->RecomputeDirty(NULL, this, NULL);
  ASSERT_FALSE(GetNode("in")->dirty_);
  ASSERT_TRUE(GetNode("mid")->dirty_);
  ASSERT_TRUE(GetNode("out")->dirty_);
}

class DiskInterfaceTest : public testing::Test {
public:
  virtual void SetUp() {
    char buf[4 << 10];
    ASSERT_TRUE(getcwd(buf, sizeof(buf)));
    start_dir_ = buf;

    char name_template[] = "DiskInterfaceTest-XXXXXX";
    char* name = mkdtemp(name_template);
    temp_dir_name_ = name;
    ASSERT_TRUE(name);
    ASSERT_EQ(0, chdir(name));
  }
  virtual void TearDown() {
    ASSERT_EQ(0, chdir(start_dir_.c_str()));
    ASSERT_EQ(0, system(("rm -rf " + temp_dir_name_).c_str()));
  }

  string start_dir_;
  string temp_dir_name_;
  RealDiskInterface disk_;
};

TEST_F(DiskInterfaceTest, Stat) {
  EXPECT_EQ(0, disk_.Stat("nosuchfile"));

  string too_long_name(512, 'x');
  EXPECT_EQ(-1, disk_.Stat(too_long_name));

  ASSERT_EQ(0, system("touch file"));
  EXPECT_GT(disk_.Stat("file"), 1);
}

TEST_F(DiskInterfaceTest, ReadFile) {
  string err;
  EXPECT_EQ("", disk_.ReadFile("foobar", &err));
  EXPECT_EQ("", err);

  const char* kTestFile = "testfile";
  FILE* f = fopen(kTestFile, "wb");
  ASSERT_TRUE(f);
  const char* kTestContent = "test content\nok";
  fprintf(f, "%s", kTestContent);
  ASSERT_EQ(0, fclose(f));

  EXPECT_EQ(kTestContent, disk_.ReadFile(kTestFile, &err));
  EXPECT_EQ("", err);
}

TEST_F(DiskInterfaceTest, MakeDirs) {
  EXPECT_TRUE(disk_.MakeDirs("path/with/double//slash/"));
}
