using System.Linq;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class CompilationStatisticsPresentationTests
{
    [Fact]
    public void GeometryStage_ShowsOnlyGeometrySpecificStatistics()
    {
        var groups = CompilationStatisticsPresentation.Groups(
            "geometry",
            new CompilationStatisticsModel
            {
                CutInstructionCount = 2,
                EmitInstructionCount = 3,
                GeometryShaderMaxOutputVertexCount = 4,
                GeometryShaderInstanceCount = 1,
            });

        var geometry = Assert.Single(groups, group => group.Title == "Geometry stage");
        Assert.Contains(
            geometry.Values,
            value => value.Key == "Maximum output vertices" && value.Value == 4);
        Assert.DoesNotContain(groups, group => group.Title == "Tessellation stage");
    }

    [Theory]
    [InlineData("hull")]
    [InlineData("domain")]
    public void TessellationStages_ShowOnlyTessellationSpecificStatistics(string stage)
    {
        var groups = CompilationStatisticsPresentation.Groups(
            stage,
            new CompilationStatisticsModel
            {
                ControlPointCount = 3,
                PatchConstantParameterCount = 2,
            });

        var tessellation =
            Assert.Single(groups, group => group.Title == "Tessellation stage");
        Assert.Contains(
            tessellation.Values,
            value => value.Key == "Patch-constant parameters" && value.Value == 2);
        Assert.DoesNotContain(groups, group => group.Title == "Geometry stage");
    }

    [Fact]
    public void PixelStage_OmitsStageSpecificStatistics()
    {
        var groups = CompilationStatisticsPresentation.Groups(
            "pixel",
            new CompilationStatisticsModel());

        Assert.Equal(
            new[]
            {
                "Overview",
                "Instruction classes",
                "Texture operations",
                "Flow and synchronization",
                "Compiler metadata",
            },
            groups.Select(group => group.Title));
    }
}
