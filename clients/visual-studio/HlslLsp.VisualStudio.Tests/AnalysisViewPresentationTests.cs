using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class AnalysisViewPresentationTests
{
    [Fact]
    public void SectionState_PreservesExplicitExpansionChoice()
    {
        var state = new AnalysisViewState();

        Assert.True(state.IsExpanded("Summary", true));
        state.SetExpanded("Summary", false);

        Assert.False(state.IsExpanded("Summary", true));
    }

    [Fact]
    public void Filter_MatchesAnyFieldWithoutCaseSensitivity()
    {
        var state = new AnalysisViewState { Filter = "texture" };

        Assert.True(state.Matches("g_Texture", "Texture2D", "srv"));
        Assert.False(state.Matches("Globals", "ConstantBuffer", "cbv"));
    }

    [Fact]
    public void EmptyFilter_MatchesAllRows()
    {
        var state = new AnalysisViewState { Filter = "  " };

        Assert.True(state.Matches(null, string.Empty));
    }
}
