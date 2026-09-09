using System;

using HlslLsp.VisualStudio.Bootstrap;

using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class VariantSelectionTests
{
    [Fact]
    public void ForContext_OmitsVariantsForOtherFiles()
    {
        var result = VariantSelection.ForContext(
            Variants(
                Variant("Current", true, "main"),
                Variant("Other", false, "otherMain")),
            null);

        var variant = Assert.Single(result.Variants);
        Assert.Equal("Current", variant.Name);
    }

    [Fact]
    public void ForContext_FiltersToTheClickedEntryPoint()
    {
        var result = VariantSelection.ForContext(
            Variants(
                Variant("First", true, "firstMain"),
                Variant("Second", true, "secondMain"),
                Variant("Second Debug", true, "secondMain")),
            "secondMain");

        Assert.Collection(
            result.Variants,
            variant => Assert.Equal("Second", variant.Name),
            variant => Assert.Equal("Second Debug", variant.Name));
    }

    [Fact]
    public void ForContext_KeepsFileVariantsWhenTheClickedFunctionIsNotAnEntryPoint()
    {
        var result = VariantSelection.ForContext(
            Variants(
                Variant("First", true, "firstMain"),
                Variant("Second", true, "secondMain")),
            "helper");

        Assert.Equal(2, result.Variants.Count);
    }

    private static VariantListModel Variants(params VariantModel[] variants)
        => new()
        {
            ActiveVariant = "First",
            Variants = variants,
        };

    private static VariantModel Variant(
        string name,
        bool applicable,
        string entryPoint)
        => new()
        {
            Name = name,
            Applicable = applicable,
            EntryPoint = entryPoint,
        };
}
