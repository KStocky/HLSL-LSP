using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

// Pure presentation-logic coverage for the Entry-Point Data Flow tool
// window, mirroring MemoryLayoutDisplayNameTests: these helpers derive
// display text only from already-known enum-like fields the server reports
// (access kind, recursion flag, depth, truncation), never from a symbol
// name or location, so they are fully testable without any WPF/VS
// dependency.
public sealed class EntryPointDataFlowDisplayTests
{
    [Theory]
    [InlineData("read", "Read")]
    [InlineData("write", "Write")]
    [InlineData("readWrite", "Read/Write")]
    [InlineData("somethingUnexpected", "somethingUnexpected")]
    [InlineData(null, "")]
    public void AccessLabel_MapsKnownKindsAndPassesThroughUnknownValues(
        string access,
        string expected)
        => Assert.Equal(expected, EntryPointDataFlowDisplay.AccessLabel(access));

    [Theory]
    [InlineData(true, "Recursive")]
    [InlineData(false, "")]
    public void RecursionLabel_OnlyLabelsRecursiveFunctions(bool recursive, string expected)
        => Assert.Equal(expected, EntryPointDataFlowDisplay.RecursionLabel(recursive));

    [Fact]
    public void DepthLabel_LabelsZeroAsEntryPoint()
        => Assert.Equal("Entry point", EntryPointDataFlowDisplay.DepthLabel(0));

    [Theory]
    [InlineData(1, "Depth 1")]
    [InlineData(4, "Depth 4")]
    public void DepthLabel_LabelsNonZeroDepthsNumerically(long depth, string expected)
        => Assert.Equal(expected, EntryPointDataFlowDisplay.DepthLabel(depth));

    [Fact]
    public void FunctionsVisitedSummary_ReportsPlainCountWhenNotTruncated()
        => Assert.Equal(
            "4 function(s) visited",
            EntryPointDataFlowDisplay.FunctionsVisitedSummary(4, functionsVisitedTruncated: false));

    [Fact]
    public void FunctionsVisitedSummary_ExplainsConservativeSubsetWhenTruncated()
    {
        var summary = EntryPointDataFlowDisplay.FunctionsVisitedSummary(
            4096,
            functionsVisitedTruncated: true);

        Assert.Contains("4096", summary);
        Assert.Contains("truncated", summary);
    }

    [Fact]
    public void GlobalAccessesTruncationWarning_IsNullWhenNotTruncated()
        => Assert.Null(EntryPointDataFlowDisplay.GlobalAccessesTruncationWarning(false));

    [Fact]
    public void GlobalAccessesTruncationWarning_WarnsAboutAccessScanSpecificallyWhenTruncated()
    {
        var warning = EntryPointDataFlowDisplay.GlobalAccessesTruncationWarning(true);

        Assert.NotNull(warning);
        Assert.Contains("access", warning);
    }

    [Fact]
    public void UnusedDeclarationsTruncationWarning_IsNullWhenNotTruncated()
        => Assert.Null(EntryPointDataFlowDisplay.UnusedDeclarationsTruncationWarning(false));

    [Fact]
    public void UnusedDeclarationsTruncationWarning_WarnsAboutUnusedScanSpecificallyWhenTruncated()
    {
        var warning = EntryPointDataFlowDisplay.UnusedDeclarationsTruncationWarning(true);

        Assert.NotNull(warning);
        Assert.Contains("unused", warning);
    }

    [Fact]
    public void UnreachableFunctionsPlaceholder_IsNoneWhenTraversalCompleted()
        => Assert.Equal(
            "(none)",
            EntryPointDataFlowDisplay.UnreachableFunctionsPlaceholder(
                functionsVisitedTruncated: false,
                definitionsTruncated: false));

    [Fact]
    public void UnreachableFunctionsPlaceholder_ExplainsIndeterminacyWhenTraversalTruncated()
    {
        var placeholder = EntryPointDataFlowDisplay.UnreachableFunctionsPlaceholder(
            functionsVisitedTruncated: true,
            definitionsTruncated: false);

        Assert.NotEqual("(none)", placeholder);
        Assert.Contains("Not determined", placeholder);
    }

    [Fact]
    public void UnreachableFunctionsPlaceholder_ExplainsIndeterminacyWhenDefinitionsTruncated()
    {
        var placeholder = EntryPointDataFlowDisplay.UnreachableFunctionsPlaceholder(
            functionsVisitedTruncated: false,
            definitionsTruncated: true);

        Assert.NotEqual("(none)", placeholder);
        Assert.Contains("Not determined", placeholder);
        Assert.Contains("definition", placeholder);
    }

    [Fact]
    public void UnreachableFunctionsPlaceholder_ExplainsBothCausesWhenBothTruncated()
    {
        var placeholder = EntryPointDataFlowDisplay.UnreachableFunctionsPlaceholder(
            functionsVisitedTruncated: true,
            definitionsTruncated: true);

        Assert.NotEqual("(none)", placeholder);
        Assert.Contains("Not determined", placeholder);
        Assert.Contains("traversal", placeholder);
        Assert.Contains("definition", placeholder);
    }

    [Fact]
    public void DefinitionsTruncatedWarning_IsNullWhenNotTruncated()
        => Assert.Null(EntryPointDataFlowDisplay.DefinitionsTruncatedWarning(false));

    [Fact]
    public void DefinitionsTruncatedWarning_WarnsAboutDefinitionCollectionSpecificallyWhenTruncated()
    {
        var warning = EntryPointDataFlowDisplay.DefinitionsTruncatedWarning(true);

        Assert.NotNull(warning);
        Assert.Contains("definition", warning);
    }

    [Fact]
    public void NotFoundMessage_PrefersServerExplanationWhenPresent()
        => Assert.Equal(
            "No entry point is configured.",
            EntryPointDataFlowDisplay.NotFoundMessage("No entry point is configured."));

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    public void NotFoundMessage_FallsBackToGenericMessageWhenExplanationIsMissing(
        string explanation)
        => Assert.Equal(
            "No entry-point data flow is available for the active document/variant.",
            EntryPointDataFlowDisplay.NotFoundMessage(explanation));

    // --- EntryPointDataFlowNavigation -------------------------------------
    // These mirror the same "prefer SelectionRange, fall back to Range"
    // contract PreprocessorExplorerToolWindow/ResourceBindingsToolWindow
    // apply elsewhere: navigation must target the symbol name, not its
    // whole declaration span, whenever the server supplied a valid,
    // non-inverted SelectionRange.

    private static CompilationSourceRangeModel Range(
        long startLine,
        long startCharacter,
        long endLine,
        long endCharacter)
        => new CompilationSourceRangeModel
        {
            Start = new CompilationSourcePositionModel
            {
                Line = startLine,
                Character = startCharacter,
            },
            End = new CompilationSourcePositionModel
            {
                Line = endLine,
                Character = endCharacter,
            },
        };

    [Fact]
    public void SelectNavigationRange_PrefersWellFormedSelectionRange()
    {
        var range = Range(10, 0, 20, 0);
        var selectionRange = Range(10, 5, 10, 12);

        var chosen = EntryPointDataFlowNavigation.SelectNavigationRange(range, selectionRange);

        Assert.Same(selectionRange, chosen);
    }

    [Fact]
    public void SelectNavigationRange_FallsBackToRangeWhenSelectionRangeIsNull()
    {
        var range = Range(10, 0, 20, 0);

        var chosen = EntryPointDataFlowNavigation.SelectNavigationRange(range, null);

        Assert.Same(range, chosen);
    }

    [Fact]
    public void SelectNavigationRange_FallsBackToRangeWhenSelectionRangeHasMissingPositions()
    {
        var range = Range(10, 0, 20, 0);
        var selectionRange = new CompilationSourceRangeModel { Start = null, End = null };

        var chosen = EntryPointDataFlowNavigation.SelectNavigationRange(range, selectionRange);

        Assert.Same(range, chosen);
    }

    [Fact]
    public void SelectNavigationRange_FallsBackToRangeWhenSelectionRangeIsInverted()
    {
        var range = Range(10, 0, 20, 0);
        var invertedSelectionRange = Range(10, 12, 10, 5);

        var chosen = EntryPointDataFlowNavigation.SelectNavigationRange(
            range,
            invertedSelectionRange);

        Assert.Same(range, chosen);
    }

    [Theory]
    [InlineData(-1L, 0L, 5L, 0L)]
    [InlineData(0L, -1L, 5L, 0L)]
    [InlineData(0L, 0L, -1L, 0L)]
    [InlineData(0L, 0L, 5L, -1L)]
    public void SelectNavigationRange_FallsBackToRangeWhenSelectionRangeHasNegativePositions(
        long startLine,
        long startCharacter,
        long endLine,
        long endCharacter)
    {
        var range = Range(10, 0, 20, 0);
        var negativeSelectionRange = Range(startLine, startCharacter, endLine, endCharacter);

        var chosen = EntryPointDataFlowNavigation.SelectNavigationRange(
            range,
            negativeSelectionRange);

        Assert.Same(range, chosen);
    }

    [Fact]
    public void SelectNavigationRange_ReturnsNullWhenBothRangesAreAbsent()
        => Assert.Null(EntryPointDataFlowNavigation.SelectNavigationRange(null, null));

    [Fact]
    public void IsWellFormedRange_AcceptsSameStartAndEndPosition()
        => Assert.True(EntryPointDataFlowNavigation.IsWellFormedRange(Range(4, 2, 4, 2)));

    [Fact]
    public void IsWellFormedRange_RejectsNullRange()
        => Assert.False(EntryPointDataFlowNavigation.IsWellFormedRange(null));
}

