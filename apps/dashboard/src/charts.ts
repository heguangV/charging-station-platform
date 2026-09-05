import { use } from 'echarts/core'
import { BarChart, LineChart, PieChart } from 'echarts/charts'
import { GridComponent, TooltipComponent, LegendComponent, TitleComponent, VisualMapComponent, MarkPointComponent } from 'echarts/components'
import { CanvasRenderer } from 'echarts/renderers'
use([BarChart, LineChart, PieChart, GridComponent, TooltipComponent, LegendComponent, TitleComponent, VisualMapComponent, MarkPointComponent, CanvasRenderer])
export { init, graphic } from 'echarts/core'
export type { ECharts, EChartsCoreOption as EChartsOption } from 'echarts/core'
