/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

/**
 
 This ProGuard lexer is designed to lex only the output of
 running -printconfiguration from ProGuard which produces a single
 ProGuard configuration file which has the total merged configuration
 for the application. This will not contain ant Ant directives like
 <java.home> which are expanded and it will not contain directives
 like -include since all the included files will have been inlined
 and merged.
 
 **/

#pragma once

#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace redex {
  namespace proguard_parser {
  
    enum class token {
      openCurlyBracket,
      closeCurlyBracket,
      openBracket,
      closeBracket,
      semiColon,
      colon,
      notToken,
      comma,
      dot,
      slash,
      classToken,
      publicToken,
      final,
      abstract,
      interface,
      enumToken,
      extends,
      implements,
      privateToken,
      protectedToken,
      staticToken,
      volatileToken,
      transient,
      annotation,
      annotation_application,
      synchronized,
      native,
      strictfp,
      synthetic,
      bridge,
      varargs,
      command,
      identifier,
      init,
      fields,
      methods,
      arrayType,
      filepath,
      target_version_token,
      filter_pattern,
      eof_token,
      
      // Input/Output Options
      include,
			basedirectory,
      injars,
      outjars,
      libraryjars,
      keepdirectories,
      target,
      
      // Keep Options
      keep,
      keepclassmembers,
      keepclasseswithmembers,
      keepnames,
      keepclassmembernames,
      keepclasseswithmembernames,
      printseeds,
      
      // Keep Option Modifiers
      includedescriptorclasses_token,
      allowshrinking_token,
      allowoptimization_token,
      allowobfuscation_token,
      
      // Shrinking Options
      dontshrink,
      printusage,
      whyareyoukeeping,
      
      // Optimization Options
      dontoptimize,
      optimizations,
      optimizationpasses,
      assumenosideeffects,
      mergeinterfacesaggressively,
      allowaccessmodification_token,
      
      // Obfuscation Options
      printmapping,
      repackageclasses,
      keepattributes,
      dontusemixedcaseclassnames_token,
      
      // Preverification Options
      dontpreverify_token,
      
      // General Options
      printconfiguration,
      dontwarn,
      verbose_token,
      
      unknownToken
    };
    
    class Token {
    public:
      token type;
      unsigned int line;
      Token(token typ, unsigned int line_number) : type{typ}, line{line_number} {}
      virtual std::string show() const { return "<token>"; }
      virtual bool is_command() const { return false; }
    };
    
    class OpenCurlyBracket : public Token {
    public:
      OpenCurlyBracket(unsigned int line_number) : Token(token::openCurlyBracket, line_number) {}
      std::string show() const override { return "{"; }
    };
    
    class CloseCurlyBracket : public Token {
    public:
      CloseCurlyBracket(unsigned int line_number) : Token(token::closeCurlyBracket, line_number) {}
      std::string show() const override { return "}"; }
    };
    
    
    class OpenBracket : public Token {
    public:
      OpenBracket(unsigned int line_number) : Token(token::openBracket, line_number) {}
      std::string show() const override { return "("; }
    };
    
    class CloseBracket : public Token {
    public:
      CloseBracket(unsigned int line_number) : Token(token::closeBracket, line_number) {}
      std::string show() const override { return ")"; }
    };
    
    class SemiColon : public Token {
    public:
      SemiColon(unsigned int line_number) : Token(token::semiColon, line_number) {}
      std::string show() const override { return ";"; }
    };

    class Colon : public Token {
    public:
      Colon(unsigned int line_number) : Token(token::colon, line_number) {}
      std::string show() const override { return ":"; }
    };
    
    class Not : public Token {
    public:
      Not(unsigned int line_number) : Token(token::notToken, line_number) {};
      std::string show() const override { return "!"; }
    };
    
    class Comma : public Token {
    public:
      Comma(unsigned int line_number) : Token(token::comma, line_number) {};
      std::string show() const override { return ","; }
    };
    
    class Dot : public Token {
    public:
      Dot(unsigned int line_number) : Token(token::dot, line_number) {};
      std::string show() const override { return "."; }
    };
    
    class Slash : public Token {
    public:
      Slash(unsigned int line_number) : Token(token::slash, line_number) {};
      std::string show() const override { return "/"; }
    };
    
    class Annotation : public Token {
    public:
      Annotation(unsigned int line_number, std::string anno) : Token(token::annotation, line_number) {};
      std::string show() const override { return "@interface"; }
    };
    
    class AnnotationApplication : public Token {
    public:
      AnnotationApplication(unsigned int line_number) : Token(token::annotation_application, line_number) {};
      std::string show() const override { return "@" ; }
    };
    
    class Class : public Token {
    public:
      Class(unsigned int line_number) : Token(token::classToken, line_number) {}
      std::string show() const override { return "class"; }
    };
    
    class Public : public Token {
    public:
      Public(unsigned int line_number) : Token(token::publicToken, line_number) {}
      std::string show() const override { return "public"; }
    };
    
    class Final : public Token {
    public:
      Final(unsigned int line_number) : Token(token::final, line_number) {}
      std::string show() const override { return "final"; }
    };
    
    class Abstract : public Token {
    public:
      Abstract(unsigned int line_number) : Token(token::abstract, line_number) {}
      std::string show() const override { return "abstract"; }
    };
    
    class Interface : public Token {
    public:
      Interface(unsigned int line_number) : Token(token::interface, line_number) {}
      std::string show() const override { return "interface"; }
    };
    
    class Enum : public Token {
    public:
      Enum(unsigned int line_number) : Token(token::enumToken, line_number) {}
      std::string show() const override { return "enum"; }
    };
    
    class Private : public Token {
    public:
      Private(unsigned int line_number) : Token(token::privateToken, line_number) {}
      std::string show() const override { return "private"; }
    };
    
    class Protected : public Token {
    public:
      Protected(unsigned int line_number) : Token(token::protectedToken, line_number) {}
      std::string show() const override { return "protected"; }
    };
    
    class Static : public Token {
    public:
      Static(unsigned int line_number) : Token(token::staticToken, line_number) {}
      std::string show() const override { return "static"; }
    };
    
    class Volatile : public Token {
    public:
      Volatile(unsigned int line_number) : Token(token::volatileToken, line_number) {}
      std::string show() const override { return "volatile"; }
    };
    
    class Transient : public Token {
    public:
      Transient(unsigned int line_number) : Token(token::transient, line_number) {}
      std::string show() const override { return "transient"; }
    };
    
    class Synchronized : public Token {
    public:
      Synchronized(unsigned int line_number) : Token(token::synchronized, line_number) {}
      std::string show() const override { return "synchronized"; }
    };
    
    class Native : public Token {
    public:
      Native(unsigned int line_number) : Token(token::native, line_number) {}
      std::string show() const override { return "native"; }
    };
    
    class Strictfp : public Token {
    public:
      Strictfp(unsigned int line_number) : Token(token::strictfp, line_number) {}
      std::string show() const override { return "strictfp"; }
    };
    
    class Synthetic : public Token {
    public:
      Synthetic(unsigned int line_number) : Token(token::synthetic, line_number) {}
      std::string show() const override { return "synthetic"; }
    };
    
    class Bridge : public Token {
    public:
      Bridge(unsigned int line_number) : Token(token::bridge, line_number) {}
      std::string show() const override { return "bridge"; }
    };
    
    class Varargs : public Token {
    public:
      Varargs(unsigned int line_number) : Token(token::varargs, line_number) {}
      std::string show() const override { return "varargs"; }
    };
    
    class Extends : public Token {
    public:
      Extends(unsigned int line_number) : Token(token::extends, line_number) {}
      std::string show() const override { return "extends"; }
    };
    
    class Implements : public Token {
    public:
      Implements(unsigned int line_number) : Token(token::implements, line_number) {}
      std::string show() const override { return "implements"; }
    };
    
    class Command : public Token {
    public:
      std::string command_name;
      Command(unsigned int line_number, std::string cmd) : Token(token::command, line_number), command_name{cmd} {}
      std::string show() const override { return "-" + command_name; }
      bool is_command() const override { return  true; }
    };
    
    class Identifier : public Token {
    public:
      std::string ident;
      Identifier(unsigned int line_number, std::string idenifier) : Token(token::identifier, line_number), ident{idenifier} {}
      std::string show() const override { return "identifier: " + ident; }
    };
    
    class Init : public Token {
    public:
      Init(unsigned int line_number) : Token(token::init, line_number) {}
      std::string show() const override { return "<init>"; }
    };
    
    class Fields : public Token {
    public:
      Fields(unsigned int line_number) : Token(token::fields, line_number) {}
      std::string show() const override { return "<fields>"; }
    };
    
    class Methods : public Token {
    public:
      Methods(unsigned int line_number) : Token(token::methods, line_number) {}
      std::string show() const override { return "<methods>"; }
    };
    
    class ArrayType : public Token {
    public:
      ArrayType(unsigned int line_number) : Token(token::arrayType, line_number) {}
      std::string show() const override { return "[]"; }
    };
    
    class Filepath : public Token {
    public:
      std::string path;
      Filepath(unsigned int line_number, std::string file) : Token(token::filepath, line_number), path{file} {}
      bool is_command() const override { return false; }
      std::string show() const override { return "filepath " + path; }
    };
    
    class Include : public Token {
    public:
      Include(unsigned int line_number) : Token(token::include, line_number) {}
      std::string show() const override { return "-include"; }
      bool is_command() const override { return true; }
    };

    class BaseDirectory : public Token {
    public:
      BaseDirectory(unsigned int line_number) : Token(token::basedirectory, line_number) {}
      std::string show() const override { return "-basedirectory"; }
      bool is_command() const override { return true; }
    };
		
    class InJars : public Token {
    public:
      InJars(unsigned int line_number) : Token(token::injars, line_number) {}
      std::string show() const override { return "-injars "; }
      bool is_command() const override { return true; }
    };
    
    class OutJars : public Token {
    public:
      OutJars(unsigned int line_number) : Token(token::outjars, line_number) {}
      std::string show() const override { return "-outjars "; }
      bool is_command() const override { return true; }
    };
    
    class LibraryJars : public Token {
    public:
      LibraryJars(unsigned int line_number) : Token(token::libraryjars, line_number)  {}
      std::string show() const override { return "-libraryjars "; }
      bool is_command() const override { return true; }
    };
    
    class PrintMapping : public Token {
    public:
      PrintMapping(unsigned int line_number) : Token(token::printmapping, line_number) {}
      std::string show() const override { return "-printmapping "; }
      bool is_command() const override { return true; }
    };
    
    class PrintConfiguration : public Token {
    public:
      PrintConfiguration(unsigned int line_number) : Token(token::printconfiguration, line_number) {}
      std::string show() const override { return "-printconfiguration "; }
      bool is_command() const override { return true; }
    };
    
    class PrintSeeds : public Token {
    public:
      PrintSeeds(unsigned int line_number) : Token(token::printseeds, line_number) {}
      std::string show() const override { return "-printseeds "; }
      bool is_command() const override { return true; }
    };
    
    class DontShrink : public Token {
    public:
      DontShrink(unsigned int line_number) : Token(token::dontshrink, line_number) {}
      std::string show() const override { return "-dontshrink"; }
      bool is_command() const override { return true; }
    };
    
    class PrintUsage : public Token {
    public:
      PrintUsage(unsigned int line_number) : Token(token::printusage, line_number) {}
      std::string show() const override { return "-printusage"; }
      bool is_command() const override { return true; }
    };
    
    class WhyAreYouKeeping : public Token {
    public:
      WhyAreYouKeeping(unsigned int line_number) : Token(token::whyareyoukeeping, line_number) {}
      std::string show() const override { return "-whyareyoukeeping"; }
      bool is_command() const override { return true; }
    };
    
    class IncludeDescriptorClasses : public Token {
    public:
      IncludeDescriptorClasses(unsigned int line_number) : Token(token::includedescriptorclasses_token, line_number) {}
      std::string show() const override { return "includedescriptorclasses"; }
    };
    
    class AllowOptimization : public Token {
    public:
      AllowOptimization(unsigned int line_number) : Token(token::allowoptimization_token, line_number) {}
      std::string show() const override { return "allowshrinking"; }
    };
    
    class AllowShrinking : public Token {
    public:
      AllowShrinking(unsigned int line_number) : Token(token::allowshrinking_token, line_number) {}
      std::string show() const override { return "allowoptimization"; }
    };
    
    class AllowObfuscation : public Token {
    public:
      AllowObfuscation(unsigned int line_number) : Token(token::allowobfuscation_token, line_number) {}
      std::string show() const override { return "allowobfuscation"; }
    };
    
    class KeepDirectories : public Token {
    public:
      KeepDirectories(unsigned int line_number) : Token(token::keepdirectories, line_number) {}
      std::string show() const override { return "-keepdirectories"; }
      bool is_command() const override { return true; }
    };
    
    class TargetVersion : public Token {
    public:
      std::string target_version;
      TargetVersion (unsigned int line_number, std::string version) : Token(token::target_version_token, line_number), target_version{version} {} ;
      std::string show() const override { return target_version; }
    };
    
    class Target : public Token {
    public:
      Target(unsigned int line_number) : Token(token::target, line_number) {}
      std::string show() const override { return "-target "; }
      bool is_command() const override { return true; }
    };
    
    class Keep : public Token {
    public:
      Keep(unsigned int line_number) : Token(token::keep, line_number) {}
      std::string show() const override { return "-keep"; }
      bool is_command() const override { return true; }
    };
    
    class KeepClassMembers : public Token {
    public:
      KeepClassMembers(unsigned int line_number) : Token(token::keepclassmembers, line_number) {}
      std::string show() const override { return "-keepclassmembers"; }
      bool is_command() const override { return true; }
    };
    
    class KeepClassesWithMembers : public Token {
    public:
      KeepClassesWithMembers(unsigned int line_number) : Token(token::keepclasseswithmembers, line_number) {}
      std::string show() const override { return "-keepclasseswithmembers"; }
      bool is_command() const override { return true; }
    };
    
    class KeepNames : public Token {
    public:
      KeepNames(unsigned int line_number) : Token(token::keepnames, line_number) {}
      std::string show() const override { return "-keepnames"; }
      bool is_command() const override { return true; }
    };
    
    class KeepClassMemberNames : public Token {
    public:
      KeepClassMemberNames(unsigned int line_number) : Token(token::keepclassmembernames, line_number) {}
      std::string show() const override { return "-keepclassmembernames"; }
      bool is_command() const override { return true; }
    };
    
    class KeepClassesWithMemberNames : public Token {
    public:
      KeepClassesWithMemberNames(unsigned int line_number) : Token(token::keepclasseswithmembernames, line_number) {}
      std::string show() const override { return "-keepclasseswithmembernames"; }
      bool is_command() const override { return true; }
    };
    
    class RepackageClasses : public Token {
    public:
      RepackageClasses(unsigned int line_number) : Token(token::repackageclasses, line_number) {}
      std::string show() const override { return "-repackageclasses"; }
      bool is_command() const override { return true; }
    };
    
    class Optimizations : public Token {
    public:
      Optimizations(unsigned int line_number) : Token(token::optimizations, line_number) {}
      std::string show() const override { return "-optimizations"; }
      bool is_command() const override { return true; }
    };
    
    class OptimizationPasses : public Token {
    public:
      OptimizationPasses(unsigned int line_number) : Token(token::optimizationpasses, line_number) {}
      std::string show() const override { return "-optimizationpasses"; }
      bool is_command() const override { return true; }
    };
    
    class Filter : public Token {
    public:
      std::string filter;
      Filter(unsigned int line_number, std::string pattern) : Token(token::filter_pattern, line_number), filter{pattern} {}
      std::string show() const override { return "filter: " + filter; }
    };
    
    class KeepAttributes : public Token {
    public:
      KeepAttributes(unsigned int line_number) : Token(token::keepattributes, line_number) {}
      std::string show() const override { return "-keepattributes"; }
      bool is_command() const override { return true; }
    };
    
    class DontWarn : public Token {
    public:
      DontWarn(unsigned int line_number) : Token(token::dontwarn, line_number) {}
      std::string show() const override { return "-dontwarn"; }
      bool is_command() const override { return true; }
    };
    
    class AssumeSideEffects : public Token {
    public:
      AssumeSideEffects(unsigned int line_number) : Token(token::assumenosideeffects, line_number) {}
      std::string show() const override { return "-assumenosideeffects"; }
      bool is_command() const override { return true; }
    };
    
    class AllowAccessModification : public Token {
    public:
      AllowAccessModification(unsigned int line_number) : Token(token::allowaccessmodification_token, line_number) {}
      std::string show() const override { return "-allowaccessmodification"; }
      bool is_command() const override { return true; }
    };
    
    class DontUseMixedcaseClassNames : public Token {
    public:
      DontUseMixedcaseClassNames(unsigned int line_number) : Token(token::dontusemixedcaseclassnames_token, line_number) {}
      std::string show() const override { return "-dontusemixedcaseclassnames"; }
      bool is_command() const override { return true; }
    };
    
    class DontOptimize : public Token {
    public:
      DontOptimize(unsigned int line_number) : Token(token::dontoptimize, line_number) {}
      std::string show() const override { return "-dontoptimize"; }
      bool is_command() const override { return true; }
    };
    
    class MergeInterfacesAggressively : public Token {
    public:
      MergeInterfacesAggressively(unsigned int line_number) : Token(token::mergeinterfacesaggressively, line_number) {}
      std::string show() const override { return "-mergeinterfacesaggressively"; }
      bool is_command() const override { return true; }
    };
    
    class DontPreverify : public Token {
    public:
      DontPreverify(unsigned int line_number) : Token(token::dontpreverify_token, line_number) {}
      std::string show() const override { return "-dontpreverify"; }
      bool is_command() const override { return true; }
    };
    
    class Verbose : public Token {
    public:
      Verbose(unsigned int line_number) : Token(token::verbose_token, line_number) {}
      std::string show() const override { return "-verbose"; }
      bool is_command() const override { return true; }
    };
    
    class UnknownToken : public Token {
    public:
      std::string token_string;
      std::string show() const override {
        return "unknown token at line " + std::to_string(line) + " : " + token_string;
      }
      UnknownToken(std::string token_text, unsigned int line_number) : Token(token::unknownToken, line_number), token_string{token_text} {}
    };
    
    class EndOfFile : public Token {
    public:
      EndOfFile(unsigned int line_number) : Token(token::eof_token, line_number) {}
      std::string show() const override { return "<EOF>"; }
    };
    
    std::vector<std::unique_ptr<Token>> lex(std::istream& config);
    
  } // namespace proguard_parser
} // namespeace redex

