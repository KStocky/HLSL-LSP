using System.ComponentModel.Composition;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Keyword)]
[Name(HlslClassificationNames.Keyword + " format")]
[UserVisible(true)]
internal sealed class HlslKeywordFormat : ClassificationFormatDefinition
{
    public HlslKeywordFormat() => DisplayName = "HLSL Keyword";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Preprocessor)]
[Name(HlslClassificationNames.Preprocessor + " format")]
[UserVisible(true)]
internal sealed class HlslPreprocessorFormat : ClassificationFormatDefinition
{
    public HlslPreprocessorFormat() => DisplayName = "HLSL Preprocessor Directive";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Function)]
[Name(HlslClassificationNames.Function + " format")]
[UserVisible(true)]
internal sealed class HlslFunctionFormat : ClassificationFormatDefinition
{
    public HlslFunctionFormat() => DisplayName = "HLSL Function";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Type)]
[Name(HlslClassificationNames.Type + " format")]
[UserVisible(true)]
internal sealed class HlslTypeFormat : ClassificationFormatDefinition
{
    public HlslTypeFormat() => DisplayName = "HLSL Type";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Comment)]
[Name(HlslClassificationNames.Comment + " format")]
[UserVisible(true)]
internal sealed class HlslCommentFormat : ClassificationFormatDefinition
{
    public HlslCommentFormat() => DisplayName = "HLSL Comment";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.String)]
[Name(HlslClassificationNames.String + " format")]
[UserVisible(true)]
internal sealed class HlslStringFormat : ClassificationFormatDefinition
{
    public HlslStringFormat() => DisplayName = "HLSL String";
}

[Export(typeof(EditorFormatDefinition))]
[ClassificationType(ClassificationTypeNames = HlslClassificationNames.Number)]
[Name(HlslClassificationNames.Number + " format")]
[UserVisible(true)]
internal sealed class HlslNumberFormat : ClassificationFormatDefinition
{
    public HlslNumberFormat() => DisplayName = "HLSL Number";
}
